/**
 * @file audioengine_openal.cpp
 * @brief implementation of audio engine using OpenAL
 * support as a OpenAL 3D implementation
 *
 * $LicenseInfo:firstyear=2002&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#include "linden_common.h"
#include "lldir.h"

#include "llaudioengine_openal.h"
#include "lllistener_openal.h"

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>

// <FS:Trish>
#include "workqueue.h"
#include <mutex>
// <FS:Trish> FSPerfAudioThread
#include <condition_variable>
#include <deque>
#if LL_WINDOWS
#include "llwin32headers.h"
#endif
#include <thread>
#include <vector>

bool LLAudioEngine_OpenAL::sFSUseAudioThread = false;

namespace
{
    // One thread making every OpenAL call, in the order the main thread asked for them. Between
    // commands, and at least every 10 ms, it refreshes the state the main thread reads of each
    // channel (playing or not, sample offset).
    class FSAudioThread
    {
    public:
        void start()
        {
            mStop = false;
            mThread = std::thread([this]() { run(); });
            mRunning.store(true);
        }

        // Runs what is queued, then stops. Later fs_audio_run() calls run in place.
        void stop()
        {
            if (!mRunning.exchange(false))
            {
                return;
            }
            {
                std::lock_guard<std::mutex> lock(mMutex);
                mStop = true;
            }
            mCv.notify_all();
            mThread.join();
        }

        bool running() const { return mRunning.load(); }

        void post(std::function<void()> fn)
        {
            {
                std::lock_guard<std::mutex> lock(mMutex);
                mQueue.push_back(std::move(fn));
            }
            mCv.notify_one();
        }

        // Audio thread
        void addChannel(LLAudioChannelOpenAL* channel) { mChannels.push_back(channel); }
        void removeChannel(LLAudioChannelOpenAL* channel)
        {
            mChannels.erase(std::remove(mChannels.begin(), mChannels.end(), channel), mChannels.end());
        }

    private:
        void run()
        {
            LL_PROFILER_SET_THREAD_NAME("FS Audio");
#if LL_WINDOWS
            SetThreadDescription(GetCurrentThread(), L"FS Audio");
#endif
            std::deque<std::function<void()>> work;
            while (true)
            {
                {
                    std::unique_lock<std::mutex> lock(mMutex);
                    if (mQueue.empty() && !mStop)
                    {
                        mCv.wait_for(lock, std::chrono::milliseconds(10));
                    }
                    work.swap(mQueue);
                    if (work.empty() && mStop)
                    {
                        break;
                    }
                }
                for (std::function<void()>& fn : work)
                {
                    fn();
                }
                work.clear();
                for (LLAudioChannelOpenAL* channel : mChannels)
                {
                    channel->fsRefreshState();
                }
            }
        }

        std::thread mThread;
        std::mutex mMutex;
        std::condition_variable mCv;
        std::deque<std::function<void()>> mQueue;
        bool mStop = false;
        std::atomic<bool> mRunning{ false };
        std::vector<LLAudioChannelOpenAL*> mChannels; // audio thread
    };

    FSAudioThread sFSAudioThread;
}

bool fs_audio_threaded()
{
    return sFSAudioThread.running();
}

void fs_audio_run(std::function<void()> fn)
{
    if (sFSAudioThread.running())
    {
        sFSAudioThread.post(std::move(fn));
    }
    else
    {
        fn();
    }
}
// </FS:Trish>

// Result of reading a WAV file on a worker thread. Shared with the job so a buffer destroyed
// while its file is still being read doesn't leave the job writing into freed memory.
struct LLAudioBufferOpenAL::LoadJob
{
    std::mutex mMutex;
    bool mDone = false;
    bool mAbandoned = false;
    ALvoid* mData = nullptr; // from alutLoadMemoryFromFile, released with free()
    ALenum mFormat = AL_NONE;
    ALsizei mSize = 0;
    ALfloat mFrequency = 0.f;
    ALenum mError = ALUT_ERROR_NO_ERROR;
};
// </FS:Trish>

const float LLAudioEngine_OpenAL::WIND_BUFFER_SIZE_SEC = 0.05f;

LLAudioEngine_OpenAL::LLAudioEngine_OpenAL()
    :
    mWindGen(NULL),
    mWindBuf(NULL),
    mWindBufFreq(0),
    mWindBufSamples(0),
    mWindBufBytes(0),
    mWindSource(AL_NONE),
    mNumEmptyWindALBuffers(MAX_NUM_WIND_BUFFERS),
    mALDevice(NULL),
    mHasReopenExt(false),
    mReopenDeviceSOFT(NULL),
    mHasSystemEventsExt(false),
    mHasEnumerateAllExt(false), // <FS:Ansariel> Output device selection
    mEventControlSOFT(NULL),
    mEventCallbackSOFT(NULL),
    mDefaultDeviceChanged(false)
{
}

// virtual
LLAudioEngine_OpenAL::~LLAudioEngine_OpenAL()
{
}

// virtual
static void fs_start_audio_thread(); // <FS:Trish/> FSPerfAudioThread

bool LLAudioEngine_OpenAL::init(void* userdata, const std::string &app_title)
{
    mWindGen = NULL;
    LLAudioEngine::init(userdata, app_title);

    if(!alutInit(NULL, NULL))
    {
        LL_WARNS() << "LLAudioEngine_OpenAL::init() ALUT initialization failed: " << alutGetErrorString (alutGetError ()) << LL_ENDL;
        return false;
    }

    LL_INFOS() << "LLAudioEngine_OpenAL::init() OpenAL successfully initialized" << LL_ENDL;

    LL_INFOS() << "OpenAL version: "
        << ll_safe_string(alGetString(AL_VERSION)) << LL_ENDL;
    LL_INFOS() << "OpenAL vendor: "
        << ll_safe_string(alGetString(AL_VENDOR)) << LL_ENDL;
    LL_INFOS() << "OpenAL renderer: "
        << ll_safe_string(alGetString(AL_RENDERER)) << LL_ENDL;

    ALint major = alutGetMajorVersion ();
    ALint minor = alutGetMinorVersion ();
    LL_INFOS() << "ALUT version: " << major << "." << minor << LL_ENDL;

    // <FS:Ansariel> Output device selection
    mALDevice = alcOpenDevice(nullptr);
    mALContext = alcCreateContext(mALDevice, nullptr);
    alcMakeContextCurrent(mALContext);
    // </FS:Ansariel>

    ALCdevice *device = alcGetContextsDevice(alcGetCurrentContext());

    alcGetIntegerv(device, ALC_MAJOR_VERSION, 1, &major);
    alcGetIntegerv(device, ALC_MINOR_VERSION, 1, &minor);
    LL_INFOS() << "ALC version: " << major << "." << minor << LL_ENDL;

    LL_INFOS() << "ALC default device: "
        << ll_safe_string(alcGetString(device,
                           ALC_DEFAULT_DEVICE_SPECIFIER))
        << LL_ENDL;

    // <FS:Ansariel> Output device selection
    //mALDevice = device;

    mHasReopenExt = alcIsExtensionPresent(mALDevice, "ALC_SOFT_reopen_device");
    if (mHasReopenExt)
    {
        mReopenDeviceSOFT = (LPALCREOPENDEVICESOFT)alcGetProcAddress(mALDevice, "alcReopenDeviceSOFT");
        mHasReopenExt = (mReopenDeviceSOFT != NULL);
    }
    LL_INFOS() << "ALC_SOFT_reopen_device " << (mHasReopenExt ? "supported" : "NOT supported") << LL_ENDL;

    mHasSystemEventsExt = alcIsExtensionPresent(mALDevice, "ALC_SOFT_system_events");
    if (mHasSystemEventsExt)
    {
        mEventControlSOFT = (LPALCEVENTCONTROLSOFT)alcGetProcAddress(mALDevice, "alcEventControlSOFT");
        mEventCallbackSOFT = (LPALCEVENTCALLBACKSOFT)alcGetProcAddress(mALDevice, "alcEventCallbackSOFT");
        mHasSystemEventsExt = (mEventControlSOFT != NULL && mEventCallbackSOFT != NULL);
    }
    LL_INFOS() << "ALC_SOFT_system_events " << (mHasSystemEventsExt ? "supported" : "NOT supported") << LL_ENDL;

    // <FS:Ansariel> Output device selection
    mHasEnumerateAllExt = alcIsExtensionPresent(nullptr, "ALC_ENUMERATE_ALL_EXT");
    LL_INFOS() << "ALC_ENUMERATE_ALL_EXT " << (mHasEnumerateAllExt ? "supported" : "NOT supported") << LL_ENDL;
    // </FS:Ansariel>

    // <FS:Trish> Output device recovery fallback
    mDefaultDeviceSpecifier = ll_safe_string(alcGetString(nullptr,
        mHasEnumerateAllExt ? ALC_DEFAULT_ALL_DEVICES_SPECIFIER : ALC_DEFAULT_DEVICE_SPECIFIER));
    // </FS:Trish>

    if (mHasReopenExt && mHasSystemEventsExt)
    {
        mEventCallbackSOFT(&LLAudioEngine_OpenAL::onDeviceEventSOFT, this);
        // <FS:Ansariel> Output device selection
        //ALCenum events[] = { ALC_EVENT_TYPE_DEFAULT_DEVICE_CHANGED_SOFT };
        //mEventControlSOFT(1, events, ALC_TRUE);
        //LL_INFOS() << "Registered for ALC default-device-change notifications" << LL_ENDL;
        ALCenum events[] = {
            ALC_EVENT_TYPE_DEVICE_ADDED_SOFT,
            ALC_EVENT_TYPE_DEVICE_REMOVED_SOFT,
            ALC_EVENT_TYPE_DEFAULT_DEVICE_CHANGED_SOFT
        };
        mEventControlSOFT(3, events, ALC_TRUE);
        LL_INFOS() << "Registered for ALC device notifications" << LL_ENDL;
        // </FS:Ansariel>
    }
    else
    {
        LL_INFOS() << "Dynamic OS default output device following is not available; "
            << "a viewer restart will be needed to pick up default device changes" << LL_ENDL;
    }

    fs_start_audio_thread(); // <FS:Trish/> FSPerfAudioThread
    return true;
}

// virtual
// <FS:Trish> FSPerfAudioThread: started once OpenAL is up (see the end of init)
static void fs_start_audio_thread()
{
    if (LLAudioEngine_OpenAL::sFSUseAudioThread && !sFSAudioThread.running())
    {
        sFSAudioThread.start();
        LL_INFOS("AudioEngine") << "FSPerfAudioThread: OpenAL calls run on the audio thread" << LL_ENDL;
    }
}
// </FS:Trish>

std::string LLAudioEngine_OpenAL::getDriverName(bool verbose)
{
    ALCdevice *device = alcGetContextsDevice(alcGetCurrentContext());
    std::ostringstream version;

    version <<
        "OpenAL";

    if (verbose)
    {
        version <<
            ", version " <<
            ll_safe_string(alGetString(AL_VERSION)) <<
            " / " <<
            ll_safe_string(alGetString(AL_VENDOR)) <<
            " / " <<
            ll_safe_string(alGetString(AL_RENDERER));

        if (device)
            version <<
                ": " <<
                ll_safe_string(alcGetString(device,
                    ALC_DEFAULT_DEVICE_SPECIFIER));
    }

    return version.str();
}

// virtual
void LLAudioEngine_OpenAL::allocateListener()
{
    mListenerp = (LLListener *) new LLListener_OpenAL();
    if(!mListenerp)
    {
        LL_WARNS() << "LLAudioEngine_OpenAL::allocateListener() Listener creation failed" << LL_ENDL;
    }
}

// virtual
void LLAudioEngine_OpenAL::idle()
{
    LLAudioEngine::idle();

    // <FS:Trish> FSPerfAudioThread: the device queries on the audio thread, and the sounds a reopen
    // stopped restarted here once it has finished there
    //pollDeviceChanges(); // <FS:Trish/> Output device recovery fallback
    if (fs_audio_threaded())
    {
        // pollDeviceChanges() keeps its own once a second timer, on the audio thread
        static LLFrameTimer s_poll_timer;
        if (s_poll_timer.checkExpirationAndReset(0.5f))
        {
            fs_audio_run([this]() { pollDeviceChanges(); });
        }
        if (mFSResumeChannels.exchange(false))
        {
            for (LLAudioChannel* channel : mChannels)
            {
                if (LLAudioChannelOpenAL* openal_channel = dynamic_cast<LLAudioChannelOpenAL*>(channel))
                {
                    openal_channel->resumeAfterDeviceReopen();
                }
            }
        }
    }
    else
    {
        pollDeviceChanges(); // <FS:Trish/> Output device recovery fallback
    }
    // </FS:Trish>

    if (mDefaultDeviceChanged)
    {
        mDefaultDeviceChanged = false;
        // <FS:Ansariel> Output device selection
        //reopenOnDefaultDevice();
        changeDevice();
        OnOutputDeviceListChanged(getDevices());
        // </FS:Ansariel>
    }
}

// <FS:Trish> Output device recovery fallback
// Some drivers never send the OpenAL device events, and OpenAL Soft doesn't reconnect a
// device whose endpoint went away. Poll once a second for a lost device, or for a changed
// system default while following it, and reuse the event path to reopen the device.
void LLAudioEngine_OpenAL::pollDeviceChanges()
{
    if (!mHasReopenExt || !mALDevice || mDefaultDeviceChanged
        || !mDeviceCheckTimer.checkExpirationAndReset(1.f)
        || LLFrameTimer::getTotalSeconds() < mNextRecoveryTime)
    {
        return;
    }

    bool device_lost = false;
    if (alcIsExtensionPresent(mALDevice, "ALC_EXT_disconnect"))
    {
        ALCint connected = ALC_TRUE;
        alcGetIntegerv(mALDevice, ALC_CONNECTED, 1, &connected);
        device_lost = (connected == ALC_FALSE);
    }

    const std::string current_default = ll_safe_string(alcGetString(nullptr,
        mHasEnumerateAllExt ? ALC_DEFAULT_ALL_DEVICES_SPECIFIER : ALC_DEFAULT_DEVICE_SPECIFIER));
    const bool default_changed = mSelectedDeviceUUID.isNull() && !current_default.empty()
        && current_default != mDefaultDeviceSpecifier;

    if (device_lost || default_changed)
    {
        LL_INFOS("OpenAL") << (device_lost ? "Output device disconnected" : "Default output device changed")
                           << " (" << mDefaultDeviceSpecifier << " -> " << current_default << "), reopening" << LL_ENDL;
        mDefaultDeviceSpecifier = current_default;
        mDefaultDeviceChanged = true;
        // Don't retrigger every second if the reopen keeps failing
        mNextRecoveryTime = LLFrameTimer::getTotalSeconds() + 5.0;
    }
}

void LLAudioChannelOpenAL::resumeAfterDeviceReopen()
{
    // OpenAL Soft stops every source when the device disconnects
    if (mCurrentSourcep && !isWaiting() && !isPlaying())
    {
        play();
    }
}
// </FS:Trish>

// static
void ALC_APIENTRY LLAudioEngine_OpenAL::onDeviceEventSOFT(ALCenum eventType, ALCenum deviceType,
                                                            ALCdevice* /*device*/, ALCsizei /*length*/,
                                                            const ALCchar* /*message*/, void* userParam) noexcept
{
    // <FS:Ansariel> Output device selection
    if (eventType == ALC_EVENT_TYPE_DEVICE_ADDED_SOFT || eventType == ALC_EVENT_TYPE_DEVICE_REMOVED_SOFT)
    {
        if (deviceType == ALC_PLAYBACK_DEVICE_SOFT)
        {
            LLAudioEngine_OpenAL* self = static_cast<LLAudioEngine_OpenAL*>(userParam);
            if (self)
            {
                LL_INFOS() << "LLAudioEngine_OpenAL:: Playback device list changed" << LL_ENDL;
                self->mDefaultDeviceChanged = true;
                return;
            }
        }
    }
    // </FS:Ansariel>

    // This may run on OpenAL's internal thread, so just flip the mDefaultDeviceChanged flag
    // and handle device change in idle().
    if (eventType == ALC_EVENT_TYPE_DEFAULT_DEVICE_CHANGED_SOFT)
    {
        LLAudioEngine_OpenAL* self = static_cast<LLAudioEngine_OpenAL*>(userParam);
        if (self)
        {
            LL_INFOS() << "LLAudioEngine_OpenAL:: Default playback device changed" << LL_ENDL;
            self->mDefaultDeviceChanged = true;
        }
    }
}

void LLAudioEngine_OpenAL::reopenOnDefaultDevice()
{
    if (!mHasReopenExt || !mReopenDeviceSOFT || !mALDevice)
    {
        return;
    }

    std::string old_name = ll_safe_string(alcGetString(mALDevice, ALC_DEVICE_SPECIFIER));
    LL_INFOS() << "OS default audio device changed; reopening OpenAL device (was: "
        << old_name << ")" << LL_ENDL;

    // Clear any prior device error state so failures below report accurately.
    (void)alcGetError(mALDevice);

    if (mReopenDeviceSOFT(mALDevice, NULL, NULL))
    {
        LL_INFOS() << "OpenAL device reopened successfully, now on: "
            << ll_safe_string(alcGetString(mALDevice, ALC_DEVICE_SPECIFIER)) << LL_ENDL;
    }
    else
    {
        ALCenum error = alcGetError(mALDevice);
        LL_WARNS() << "alcReopenDeviceSOFT() failed: alc error " << error << LL_ENDL;
    }
}

// virtual
void LLAudioEngine_OpenAL::shutdown()
{
    LL_INFOS() << "About to LLAudioEngine::shutdown()" << LL_ENDL;

    sFSAudioThread.stop(); // <FS:Trish/> FSPerfAudioThread: runs what is queued; the rest of shutdown runs here

    if (mHasReopenExt && mHasSystemEventsExt && mEventControlSOFT && mEventCallbackSOFT)
    {
        // <FS:Ansariel> Output device selection
        //ALCenum events[] = { ALC_EVENT_TYPE_DEFAULT_DEVICE_CHANGED_SOFT };
        //mEventControlSOFT(1, events, ALC_FALSE);
        ALCenum events[] = {
            ALC_EVENT_TYPE_DEVICE_ADDED_SOFT,
            ALC_EVENT_TYPE_DEVICE_REMOVED_SOFT,
            ALC_EVENT_TYPE_DEFAULT_DEVICE_CHANGED_SOFT
        };
        mEventControlSOFT(3, events, ALC_FALSE);
        // </FS:Ansariel>
        mEventCallbackSOFT(NULL, NULL);
    }

    LLAudioEngine::shutdown();

    // If a subsequent error occurs while there is still an error recorded
    // internally, the second error will simply be ignored.
    // Clear previous error to make sure we will captuare a valid failure reason
    ALenum error = alutGetError();
    if (error != ALUT_ERROR_NO_ERROR)
    {
        LL_WARNS() << "Uncleared error state prior to shutdown: "
            << alutGetErrorString(error) << LL_ENDL;
    }

    // Mac OS' alutExit can trip over stale error state. Drain it.
    if (alcGetCurrentContext() != NULL)
    {
        // Avoid potential infinite loops.
        for (int i = 0; i < 16; ++i)
        {
            if (alGetError() == AL_NO_ERROR)
            {
                break;
            }
        }
        ALenum error = alGetError();
        if (error != AL_NO_ERROR)
        {
             LL_WARNS() << "Uncleared error state prior to shutdown: " << error << LL_ENDL;
        }
    }

    LL_INFOS() << "About to alutExit()" << LL_ENDL;
    if(!alutExit())
    {
        LL_WARNS() << "LLAudioEngine_OpenAL::shutdown() ALUT shutdown failed: " << alutGetErrorString (alutGetError ()) << LL_ENDL;
    }

    LL_INFOS() << "LLAudioEngine_OpenAL::shutdown() OpenAL successfully shut down" << LL_ENDL;

    delete mListenerp;
    mListenerp = NULL;
}

LLAudioBuffer *LLAudioEngine_OpenAL::createBuffer()
{
    return new LLAudioBufferOpenAL();
}

LLAudioChannel *LLAudioEngine_OpenAL::createChannel()
{
    return new LLAudioChannelOpenAL();
}

void LLAudioEngine_OpenAL::setInternalGain(F32 gain)
{
    //LL_INFOS() << "LLAudioEngine_OpenAL::setInternalGain() Gain: " << gain << LL_ENDL;
    //alListenerf(AL_GAIN, gain);
    fs_audio_run([gain]() { alListenerf(AL_GAIN, gain); }); // <FS:Trish/> FSPerfAudioThread
}

LLAudioChannelOpenAL::LLAudioChannelOpenAL()
    :
    mALSource(AL_NONE),
    mLastSamplePos(0)
{
    // <FS:Trish> FSPerfAudioThread: created and from then on used on the audio thread
    //alGenSources(1, &mALSource);
    if (fs_audio_threaded())
    {
        fs_audio_run([this]()
            {
                alGenSources(1, &mALSource);
                sFSAudioThread.addChannel(this);
            });
        return;
    }
    alGenSources(1, &mALSource);
    // </FS:Trish>
}

LLAudioChannelOpenAL::~LLAudioChannelOpenAL()
{
    // <FS:Trish> FSPerfAudioThread: torn down there, and waited for, as the commands queued for it
    // point at this channel
    if (fs_audio_threaded())
    {
        mCurrentBufferp = NULL;
        std::shared_ptr<std::atomic<bool>> done = std::make_shared<std::atomic<bool>>(false);
        fs_audio_run([this, done]()
            {
                sFSAudioThread.removeChannel(this);
                alSourceStop(mALSource);
                alSourcei(mALSource, AL_BUFFER, AL_NONE);
                alDeleteSources(1, &mALSource);
                done->store(true);
            });
        while (!done->load() && fs_audio_threaded())
        {
            std::this_thread::yield();
        }
        return;
    }
    // </FS:Trish>
    cleanup();
    alDeleteSources(1, &mALSource);
}

// <FS:Trish> FSPerfAudioThread
void LLAudioChannelOpenAL::fsRun(std::function<void()> fn, bool state_change, bool expect_playing)
{
    if (!fs_audio_threaded())
    {
        fn();
        return;
    }
    if (!state_change)
    {
        fs_audio_run(std::move(fn));
        return;
    }
    const U32 serial = ++mFSSerial;
    mFSExpectPlaying = expect_playing;
    fs_audio_run([this, fn = std::move(fn), serial]()
        {
            fn();
            fsRefreshState();
            mFSDoneSerial.store(serial, std::memory_order_release);
        });
}

void LLAudioChannelOpenAL::fsRefreshState()
{
    if (mALSource == AL_NONE)
    {
        return;
    }
    ALint state = AL_INITIAL;
    ALint offset = 0;
    alGetSourcei(mALSource, AL_SOURCE_STATE, &state);
    alGetSourcei(mALSource, AL_SAMPLE_OFFSET, &offset);
    mFSState.store(state, std::memory_order_relaxed);
    mFSSampleOffset.store(offset, std::memory_order_relaxed);
}
// </FS:Trish>

void LLAudioChannelOpenAL::cleanup()
{
    // <FS:Trish> FSPerfAudioThread
    //alSourceStop(mALSource);
    //alSourcei(mALSource, AL_BUFFER, AL_NONE);
    fsRun([this]()
        {
            alSourceStop(mALSource);
            alSourcei(mALSource, AL_BUFFER, AL_NONE);
        }, true, false);
    // </FS:Trish>

    mCurrentBufferp = NULL;
}

void LLAudioChannelOpenAL::play()
{
    // <FS:Trish> FSPerfAudioThread: the source is the audio thread's, created there before anything else
    //if (mALSource == AL_NONE)
    if (!fs_audio_threaded() && mALSource == AL_NONE)
    // </FS:Trish>
    {
        LL_WARNS() << "Playing without a mALSource, aborting" << LL_ENDL;
        return;
    }

    if(!isPlaying())
    {
        //alSourcePlay(mALSource);
        fsRun([this]() { alSourcePlay(mALSource); }, true, true); // <FS:Trish/>
        getSource()->setPlayedOnce(true);
    }
}

void LLAudioChannelOpenAL::playSynced(LLAudioChannel *channelp)
{
    if (channelp)
    {
        LLAudioChannelOpenAL *masterchannelp =
            (LLAudioChannelOpenAL*)channelp;
        // <FS:Trish> FSPerfAudioThread: read and set the offset there, where the sources live
        if (fs_audio_threaded())
        {
            fsRun([this, masterchannelp]()
                {
                    if (mALSource != AL_NONE && masterchannelp->mALSource != AL_NONE)
                    {
                        ALfloat master_offset = 0.f;
                        alGetSourcef(masterchannelp->mALSource, AL_SEC_OFFSET, &master_offset);
                        alSourcef(mALSource, AL_SEC_OFFSET, master_offset);
                    }
                }, false);
        }
        else
        // </FS:Trish>
        if (mALSource != AL_NONE &&
            masterchannelp->mALSource != AL_NONE)
        {
            // we have channels allocated to master and slave
            ALfloat master_offset;
            alGetSourcef(masterchannelp->mALSource, AL_SEC_OFFSET,
                     &master_offset);

            LL_INFOS() << "Syncing with master at " << master_offset
                << "sec" << LL_ENDL;
            // *TODO: detect when this fails, maybe use AL_SAMPLE_
            alSourcef(mALSource, AL_SEC_OFFSET, master_offset);
        }
    }
    play();
}

bool LLAudioChannelOpenAL::isPlaying()
{
    // <FS:Trish> FSPerfAudioThread: the last state the audio thread saw, or the one asked for since
    if (fs_audio_threaded())
    {
        if (mFSDoneSerial.load(std::memory_order_acquire) != mFSSerial)
        {
            return mFSExpectPlaying;
        }
        return mFSState.load(std::memory_order_relaxed) == AL_PLAYING;
    }
    // </FS:Trish>
    if (mALSource != AL_NONE)
    {
        ALint state;
        alGetSourcei(mALSource, AL_SOURCE_STATE, &state);
        if(state == AL_PLAYING)
        {
            return true;
        }
    }

    return false;
}

bool LLAudioChannelOpenAL::updateBuffer()
{
    if (!mCurrentSourcep)
    {
        // This channel isn't associated with any source, nothing
        // to be updated
        return false;
    }

    if (LLAudioChannel::updateBuffer())
    {
        // Base class update returned true, which means that we need to actually
        // set up the source for a different buffer.
        LLAudioBufferOpenAL *bufferp = (LLAudioBufferOpenAL *)mCurrentSourcep->getCurrentBuffer();
        ALuint buffer = bufferp->getBuffer();
        // <FS:Trish> FSPerfAudioThread: a buffer made there has its id in its cell by the time this runs
        //alSourcei(mALSource, AL_BUFFER, buffer);
        std::shared_ptr<std::atomic<ALuint>> cell = bufferp->mFSBufferCell;
        fsRun([this, buffer, cell]()
            {
                alSourcei(mALSource, AL_BUFFER, cell ? cell->load() : buffer);
            }, true, false);
        // </FS:Trish>
        mLastSamplePos = 0;
    }

    if (mCurrentSourcep)
    {
        // <FS:Trish> FSPerfAudioThread
        //alSourcef(mALSource, AL_GAIN,
        //      mCurrentSourcep->getGain() * getSecondaryGain());
        //alSourcei(mALSource, AL_LOOPING,
        //      mCurrentSourcep->isLoop() ? AL_TRUE : AL_FALSE);
        //alSourcef(mALSource, AL_ROLLOFF_FACTOR,
        //      gAudiop->mListenerp->getRolloffFactor());
        const F32 gain = mCurrentSourcep->getGain() * getSecondaryGain();
        const ALint looping = mCurrentSourcep->isLoop() ? AL_TRUE : AL_FALSE;
        const F32 rolloff = gAudiop->mListenerp->getRolloffFactor();
        fsRun([this, gain, looping, rolloff]()
            {
                alSourcef(mALSource, AL_GAIN, gain);
                alSourcei(mALSource, AL_LOOPING, looping);
                alSourcef(mALSource, AL_ROLLOFF_FACTOR, rolloff);
            }, false);
        // </FS:Trish>
    }

    return true;
}


void LLAudioChannelOpenAL::updateLoop()
{
    //if (mALSource == AL_NONE)
    if (!fs_audio_threaded() && mALSource == AL_NONE) // <FS:Trish/> FSPerfAudioThread
    {
        return;
    }

    // Hack:  We keep track of whether we looped or not by seeing when the
    // sample position looks like it's going backwards.  Not reliable; may
    // yield false negatives.
    //
    ALint cur_pos;
    // <FS:Trish> FSPerfAudioThread: as last seen on the audio thread
    //alGetSourcei(mALSource, AL_SAMPLE_OFFSET, &cur_pos);
    if (fs_audio_threaded())
    {
        cur_pos = mFSSampleOffset.load(std::memory_order_relaxed);
    }
    else
    {
        alGetSourcei(mALSource, AL_SAMPLE_OFFSET, &cur_pos);
    }
    // </FS:Trish>
    if (cur_pos < mLastSamplePos)
    {
        mLoopedThisFrame = true;
    }
    mLastSamplePos = cur_pos;
}


void LLAudioChannelOpenAL::update3DPosition()
{
    if(!mCurrentSourcep)
    {
        return;
    }
    // <FS:Trish> FSPerfAudioThread: the values taken here, set there
    //if (mCurrentSourcep->isForcedPriority())
    //{
    //    alSource3f(mALSource, AL_POSITION, 0.0, 0.0, 0.0);
    //    alSource3f(mALSource, AL_VELOCITY, 0.0, 0.0, 0.0);
    //    alSourcei (mALSource, AL_SOURCE_RELATIVE, AL_TRUE);
    //} else {
    //    LLVector3 float_pos;
    //    float_pos.setVec(mCurrentSourcep->getPositionGlobal());
    //    alSourcefv(mALSource, AL_POSITION, float_pos.mV);
    //    alSourcefv(mALSource, AL_VELOCITY, mCurrentSourcep->getVelocity().mV);
    //    alSourcei (mALSource, AL_SOURCE_RELATIVE, AL_FALSE);
    //}
    //
    //alSourcef(mALSource, AL_GAIN, mCurrentSourcep->getGain() * getSecondaryGain());
    const bool forced = mCurrentSourcep->isForcedPriority();
    LLVector3 float_pos;
    LLVector3 velocity;
    if (!forced)
    {
        float_pos.setVec(mCurrentSourcep->getPositionGlobal());
        velocity = mCurrentSourcep->getVelocity();
    }
    const F32 gain = mCurrentSourcep->getGain() * getSecondaryGain();
    fsRun([this, forced, float_pos, velocity, gain]()
        {
            if (forced)
            {
                alSource3f(mALSource, AL_POSITION, 0.0, 0.0, 0.0);
                alSource3f(mALSource, AL_VELOCITY, 0.0, 0.0, 0.0);
                alSourcei (mALSource, AL_SOURCE_RELATIVE, AL_TRUE);
            }
            else
            {
                alSourcefv(mALSource, AL_POSITION, float_pos.mV);
                alSourcefv(mALSource, AL_VELOCITY, velocity.mV);
                alSourcei (mALSource, AL_SOURCE_RELATIVE, AL_FALSE);
            }
            alSourcef(mALSource, AL_GAIN, gain);
        }, false);
    // </FS:Trish>
}

LLAudioBufferOpenAL::LLAudioBufferOpenAL()
{
    mALBuffer = AL_NONE;
}

LLAudioBufferOpenAL::~LLAudioBufferOpenAL()
{
    cleanup();
}

void LLAudioBufferOpenAL::cleanup()
{
    // <FS:Trish> Let a running read clean up after itself
    if (mLoadJob)
    {
        std::lock_guard<std::mutex> lock(mLoadJob->mMutex);
        if (mLoadJob->mDone)
        {
            free(mLoadJob->mData);
            mLoadJob->mData = nullptr;
        }
        mLoadJob->mAbandoned = true;
    }
    mLoadJob.reset();
    // </FS:Trish>

    // <FS:Trish> FSPerfAudioThread: deleted there, after the commands still using it
    if (mFSBufferCell)
    {
        std::shared_ptr<std::atomic<ALuint>> cell = mFSBufferCell;
        mFSBufferCell.reset();
        fs_audio_run([cell]()
            {
                ALuint id = cell->load();
                if (id != AL_NONE)
                {
                    alDeleteBuffers(1, &id);
                }
            });
    }
    mFSReady = false;
    mFSSizeBytes = 0;
    // </FS:Trish>

    if(mALBuffer != AL_NONE)
    {
        alGetError(); // <ND/>
        alDeleteBuffers(1, &mALBuffer);

        // <FS:ND> Print warning on possible leak.
        ALenum error = alGetError();
        if( AL_NO_ERROR != error )
            LL_WARNS() << "openal error: " << error << " possible memory leak hit" << LL_ENDL;
        // </FS:ND>

        mALBuffer = AL_NONE;
    }
}

bool LLAudioBufferOpenAL::loadWAV(const std::string& filename)
{
    cleanup();

    // <FS:Trish> Reading and parsing the whole file on the main thread hitched every time a sound
    // played for the first time, like a friend logging on. Do that on a worker, getLoadState()
    // then only copies the samples into the OpenAL buffer.
    if (auto queue = LL::WorkQueue::getInstance("General"))
    {
        auto job = std::make_shared<LoadJob>();
        const bool posted = queue->post([job, filename]()
            {
                ALenum format = AL_NONE;
                ALsizei size = 0;
                ALfloat frequency = 0.f;
                ALvoid* data = alutLoadMemoryFromFile(filename.c_str(), &format, &size, &frequency);
                const ALenum error = data ? ALUT_ERROR_NO_ERROR : alutGetError();

                std::lock_guard<std::mutex> lock(job->mMutex);
                if (job->mAbandoned)
                {
                    free(data);
                    return;
                }
                job->mData = data;
                job->mFormat = format;
                job->mSize = size;
                job->mFrequency = frequency;
                job->mError = error;
                job->mDone = true;
            });
        if (posted)
        {
            mLoadJob = job;
            mLoadingFilename = filename;
            return true;
        }
    }
    // Shutting down: load in place
    // </FS:Trish>

    mALBuffer = alutCreateBufferFromFile(filename.c_str());
    if(mALBuffer == AL_NONE)
    {
        ALenum error = alutGetError();
        if (gDirUtilp->fileExists(filename))
        {
            LL_WARNS() <<
                "LLAudioBufferOpenAL::loadWAV() Error loading "
                << filename
                << " " << alutGetErrorString(error) << LL_ENDL;
        }
        else
        {
            // It's common for the file to not actually exist.
            LL_DEBUGS() <<
                "LLAudioBufferOpenAL::loadWAV() Error loading "
                 << filename
                 << " " << alutGetErrorString(error) << LL_ENDL;
        }
        return false;
    }

    return true;
}

// <FS:Trish>
LLAudioBuffer::ELoadState LLAudioBufferOpenAL::getLoadState()
{
    if (!mLoadJob)
    {
        //return mALBuffer != AL_NONE ? LOAD_READY : LOAD_FAILED;
        return (mALBuffer != AL_NONE || mFSReady) ? LOAD_READY : LOAD_FAILED; // <FS:Trish/> FSPerfAudioThread
    }

    ALvoid* data = nullptr;
    ALenum format = AL_NONE;
    ALsizei size = 0;
    ALfloat frequency = 0.f;
    ALenum load_error = ALUT_ERROR_NO_ERROR;
    {
        std::lock_guard<std::mutex> lock(mLoadJob->mMutex);
        if (!mLoadJob->mDone)
        {
            return LOAD_PENDING;
        }
        data = mLoadJob->mData;
        mLoadJob->mData = nullptr;
        format = mLoadJob->mFormat;
        size = mLoadJob->mSize;
        frequency = mLoadJob->mFrequency;
        load_error = mLoadJob->mError;
    }
    mLoadJob.reset();

    if (!data)
    {
        if (gDirUtilp->fileExists(mLoadingFilename))
        {
            LL_WARNS() << "LLAudioBufferOpenAL::loadWAV() Error loading " << mLoadingFilename
                       << " " << alutGetErrorString(load_error) << LL_ENDL;
        }
        else
        {
            // It's common for the file to not actually exist.
            LL_DEBUGS() << "LLAudioBufferOpenAL::loadWAV() Error loading " << mLoadingFilename
                        << " " << alutGetErrorString(load_error) << LL_ENDL;
        }
        return LOAD_FAILED;
    }

    // <FS:Trish> FSPerfAudioThread: the buffer made and filled there; ready here at once, since the
    // commands using it run after
    if (fs_audio_threaded())
    {
        std::shared_ptr<std::atomic<ALuint>> cell = std::make_shared<std::atomic<ALuint>>(AL_NONE);
        const std::string filename = mLoadingFilename;
        fs_audio_run([cell, data, format, size, frequency, filename]()
            {
                ALuint id = AL_NONE;
                alGetError();
                alGenBuffers(1, &id);
                if (alGetError() == AL_NO_ERROR)
                {
                    alBufferData(id, format, data, size, (ALsizei)frequency);
                    const ALenum error = alGetError();
                    if (error != AL_NO_ERROR)
                    {
                        LL_WARNS() << "LLAudioBufferOpenAL::getLoadState() OpenAL error " << error << " for " << filename << LL_ENDL;
                        alDeleteBuffers(1, &id);
                        id = AL_NONE;
                    }
                }
                free(data);
                cell->store(id);
            });
        mFSBufferCell = cell;
        mFSReady = true;
        mFSSizeBytes = (U32)size;
        return LOAD_READY;
    }
    // </FS:Trish>

    alGetError();
    alGenBuffers(1, &mALBuffer);
    if (alGetError() == AL_NO_ERROR)
    {
        alBufferData(mALBuffer, format, data, size, (ALsizei)frequency);
    }
    free(data);

    ALenum error = alGetError();
    if (error != AL_NO_ERROR)
    {
        LL_WARNS() << "LLAudioBufferOpenAL::getLoadState() OpenAL error " << error << " for " << mLoadingFilename << LL_ENDL;
        if (mALBuffer != AL_NONE)
        {
            alDeleteBuffers(1, &mALBuffer);
            mALBuffer = AL_NONE;
        }
        return LOAD_FAILED;
    }
    return LOAD_READY;
}
// </FS:Trish>

U32 LLAudioBufferOpenAL::getLength()
{
    // <FS:Trish> FSPerfAudioThread: from the size it was filled with
    if (mFSReady)
    {
        return mFSSizeBytes / 2;
    }
    // </FS:Trish>
    if(mALBuffer == AL_NONE)
    {
        return 0;
    }
    ALint length;
    alGetBufferi(mALBuffer, AL_SIZE, &length);
    return length / 2; // convert size in bytes to size in (16-bit) samples
}

// ------------

// <FS:Trish> FSPerfAudioThread: the wind's source, generator and buffer belong to the audio thread, which
// sets them up, updates and tears them down in order; the main thread only knows whether it is on
bool LLAudioEngine_OpenAL::initWind()
{
    if (fs_audio_threaded())
    {
        fs_audio_run([this]() { fsInitWind(); });
        mFSWindReady = true;
        return true;
    }
    mFSWindReady = fsInitWind();
    return mFSWindReady;
}

bool LLAudioEngine_OpenAL::fsInitWind()
// </FS:Trish>
{
    ALenum error;
    LL_INFOS() << "LLAudioEngine_OpenAL::initWind() start" << LL_ENDL;

    mNumEmptyWindALBuffers = MAX_NUM_WIND_BUFFERS;

    alGetError(); /* clear error */

    alGenSources(1,&mWindSource);

    if((error=alGetError()) != AL_NO_ERROR)
    {
        LL_WARNS() << "LLAudioEngine_OpenAL::initWind() Error creating wind sources: "<<error<<LL_ENDL;
    }

    mWindGen = new LLWindGen<WIND_SAMPLE_T>;

    mWindBufFreq = mWindGen->getInputSamplingRate();
    mWindBufSamples = llceil(mWindBufFreq * WIND_BUFFER_SIZE_SEC);
    mWindBufBytes = mWindBufSamples * 2 /*stereo*/ * sizeof(WIND_SAMPLE_T);

    mWindBuf = new WIND_SAMPLE_T [mWindBufSamples * 2 /*stereo*/];

    if(mWindBuf==NULL)
    {
        LL_ERRS() << "LLAudioEngine_OpenAL::initWind() Error creating wind memory buffer" << LL_ENDL;
        return false;
    }

    LL_INFOS() << "LLAudioEngine_OpenAL::initWind() done" << LL_ENDL;

    return true;
}

void LLAudioEngine_OpenAL::cleanupWind()
{
    LL_INFOS() << "LLAudioEngine_OpenAL::cleanupWind()" << LL_ENDL;

    // <FS:Trish> FSPerfAudioThread: the source, generator and buffer belong to the audio thread, which
    // tears them down after the wind updates still queued
    mFSWindReady = false;
    if (fs_audio_threaded())
    {
        fs_audio_run([this]() { fsDestroyWind(); });
        return;
    }
    fsDestroyWind();
}

void LLAudioEngine_OpenAL::fsDestroyWind()
{
    // </FS:Trish>
    if (mWindSource != AL_NONE)
    {
        // detach and delete all outstanding buffers on the wind source
        alSourceStop(mWindSource);
        ALint processed;
        alGetSourcei(mWindSource, AL_BUFFERS_PROCESSED, &processed);
        while (processed--)
        {
            ALuint buffer = AL_NONE;
            alSourceUnqueueBuffers(mWindSource, 1, &buffer);
            alDeleteBuffers(1, &buffer);
        }

        // delete the wind source itself
        alDeleteSources(1, &mWindSource);

        mWindSource = AL_NONE;
    }

    delete[] mWindBuf;
    mWindBuf = NULL;

    delete mWindGen;
    mWindGen = NULL;
}

void LLAudioEngine_OpenAL::updateWind(LLVector3 wind_vec, F32 camera_altitude)
{
    LLVector3 wind_pos;
    F64 pitch;
    F64 center_freq;
    //ALenum error; // <FS:Trish/> FSPerfAudioThread: in fsUpdateWindAL now

    if (!mEnableWind)
        return;

    //if(!mWindBuf)
    if (!mFSWindReady) // <FS:Trish/> FSPerfAudioThread: mWindBuf is the audio thread's
        return;

    if (mWindUpdateTimer.checkExpirationAndReset(LL_WIND_UPDATE_INTERVAL))
    {

        // wind comes in as Linden coordinate (+X = forward, +Y = left, +Z = up)
        // need to convert this to the conventional orientation DS3D and OpenAL use
        // where +X = right, +Y = up, +Z = backwards

        wind_vec.setVec(-wind_vec.mV[1], wind_vec.mV[2], -wind_vec.mV[0]);

        pitch = 1.0 + mapWindVecToPitch(wind_vec);
        center_freq = 80.0 * pow(pitch,2.5*(mapWindVecToGain(wind_vec)+1.0));

        // <FS:Trish> FSPerfAudioThread: the generator is the audio thread's, and so are the source and
        // its buffers: the targets go there with the update
        //mWindGen->mTargetFreq = (F32)center_freq;
        //mWindGen->mTargetGain = (F32)mapWindVecToGain(wind_vec) * mMaxWindGain;
        //mWindGen->mTargetPanGainR = (F32)mapWindVecToPan(wind_vec);
        const F32 freq = (F32)center_freq;
        const F32 gain = (F32)mapWindVecToGain(wind_vec) * mMaxWindGain;
        const F32 pan = (F32)mapWindVecToPan(wind_vec);
        fs_audio_run([this, freq, gain, pan]()
            {
                if (mWindGen)
                {
                    mWindGen->mTargetFreq = freq;
                    mWindGen->mTargetGain = gain;
                    mWindGen->mTargetPanGainR = pan;
                }
                fsUpdateWindAL(true);
            });
    }
    else
    {
        fs_audio_run([this]() { fsUpdateWindAL(false); });
    }
}

void LLAudioEngine_OpenAL::fsUpdateWindAL(bool set_params)
{
    ALenum error;
    if (!mWindGen || !mWindBuf || mWindSource == AL_NONE)
    {
        return;
    }
    if (set_params)
    {
    // </FS:Trish>
        alSourcei(mWindSource, AL_LOOPING, AL_FALSE);
        alSource3f(mWindSource, AL_POSITION, 0.0, 0.0, 0.0);
        alSource3f(mWindSource, AL_VELOCITY, 0.0, 0.0, 0.0);
        alSourcef(mWindSource, AL_ROLLOFF_FACTOR, 0.0);
        alSourcei(mWindSource, AL_SOURCE_RELATIVE, AL_TRUE);
    }

    // ok lets make a wind buffer now

    ALint processed, queued, unprocessed;
    alGetSourcei(mWindSource, AL_BUFFERS_PROCESSED, &processed);
    alGetSourcei(mWindSource, AL_BUFFERS_QUEUED, &queued);
    unprocessed = queued - processed;

    // ensure that there are always at least 3x as many filled buffers
    // queued as we managed to empty since last time.
    mNumEmptyWindALBuffers = llmin(mNumEmptyWindALBuffers + processed * 3 - unprocessed, MAX_NUM_WIND_BUFFERS-unprocessed);
    mNumEmptyWindALBuffers = llmax(mNumEmptyWindALBuffers, 0);

    //LL_INFOS() << "mNumEmptyWindALBuffers: " << mNumEmptyWindALBuffers    <<" (" << unprocessed << ":" << processed << ")" << LL_ENDL;

    while(processed--) // unqueue old buffers
    {
        ALuint buffer;
        ALenum error;
        alGetError(); /* clear error */
        alSourceUnqueueBuffers(mWindSource, 1, &buffer);
        error = alGetError();
        if(error != AL_NO_ERROR)
        {
            LL_WARNS() << "LLAudioEngine_OpenAL::updateWind() error swapping (unqueuing) buffers" << LL_ENDL;
        }
        else
        {
            alDeleteBuffers(1, &buffer);
        }
    }

    unprocessed += mNumEmptyWindALBuffers;
    while (mNumEmptyWindALBuffers > 0) // fill+queue new buffers
    {
        ALuint buffer;
        alGetError(); /* clear error */
        alGenBuffers(1,&buffer);
        if((error=alGetError()) != AL_NO_ERROR)
        {
            LL_WARNS() << "LLAudioEngine_OpenAL::updateWind() Error creating wind buffer: " << error << LL_ENDL;
            break;
        }

        alBufferData(buffer,
                 AL_FORMAT_STEREO_FLOAT32,
                 mWindGen->windGenerate(mWindBuf,
                            mWindBufSamples),
                 mWindBufBytes,
                 mWindBufFreq);
        error = alGetError();
        if(error != AL_NO_ERROR)
        {
            LL_WARNS() << "LLAudioEngine_OpenAL::updateWind() error swapping (bufferdata) buffers" << LL_ENDL;
        }

        alSourceQueueBuffers(mWindSource, 1, &buffer);
        error = alGetError();
        if(error != AL_NO_ERROR)
        {
            LL_WARNS() << "LLAudioEngine_OpenAL::updateWind() error swapping (queuing) buffers" << LL_ENDL;
        }

        --mNumEmptyWindALBuffers;
    }

    ALint playing;
    alGetSourcei(mWindSource, AL_SOURCE_STATE, &playing);
    if(playing != AL_PLAYING)
    {
        alSourcePlay(mWindSource);

        LL_DEBUGS() << "Wind had stopped - probably ran out of buffers - restarting: " << (unprocessed+mNumEmptyWindALBuffers) << " now queued." << LL_ENDL;
    }
}

// <FS:Ansariel> Output device selection
static LLUUID generate_uuid_from_string(const std::string& str)
{
    // Setup standard DNS namespace
    boost::uuids::string_generator string_generator;
    boost::uuids::uuid dns_namespace = string_generator("6144c9de-e8f3-4603-b8f7-a195db46f0c0");

    // Setup SHA1 name generator with the namespace
    boost::uuids::name_generator_sha1 uuid_generator(dns_namespace);

    // Generate deterministic UUID based on a string
    boost::uuids::uuid deterministic_uuid = uuid_generator(str);

    return LLUUID(boost::uuids::to_string(deterministic_uuid));
}

void LLAudioEngine_OpenAL::changeDevice()
{
    if (!mHasReopenExt || !mReopenDeviceSOFT || !mALDevice || !mALContext)
    {
        return;
    }

    std::optional<std::string> device_name{};

    if (mSelectedDeviceUUID.isNull())
    {
        LL_INFOS() << "LLAudioEngine_OpenAL::setDevice(): Setting new output to system default (" << mSelectedDeviceUUID.asString() << ")" << LL_ENDL;
    }
    else
    {
        for (const auto& [id, name] : getDevices())
        {
            if (id == mSelectedDeviceUUID)
            {
                LL_INFOS() << "LLAudioEngine_OpenAL::setDevice(): Setting new output " << name << " (" << id.asString() << ")" << LL_ENDL;
                device_name = name;
                break;
            }
        }

        if (!device_name.has_value())
        {
            LL_WARNS() << "LLAudioEngine_OpenAL::setDevice(): Output device not found - falling back to system default" << LL_ENDL;
        }
    }

    // <FS:Trish> FSPerfAudioThread: reopening can take a while, there; the sounds it stopped are
    // restarted here in idle() once it has finished
    if (fs_audio_threaded())
    {
        fs_audio_run([this, device_name]() { changeDeviceAL(device_name); });
        return;
    }
    changeDeviceAL(device_name);
    if (mFSResumeChannels.exchange(false))
    for (LLAudioChannel* channel : mChannels)
    {
        if (LLAudioChannelOpenAL* openal_channel = dynamic_cast<LLAudioChannelOpenAL*>(channel))
        {
            openal_channel->resumeAfterDeviceReopen();
        }
    }
}

void LLAudioEngine_OpenAL::changeDeviceAL(std::optional<std::string> device_name)
{
    // </FS:Trish>
    if (mReopenDeviceSOFT(mALDevice, device_name.has_value() ? device_name.value().c_str() : nullptr, nullptr))
    {
        alcMakeContextCurrent(mALContext);

        // <FS:Trish> Output device recovery fallback: restart sounds stopped by a disconnect
        //for (LLAudioChannel* channel : mChannels)
        //{
        //    if (LLAudioChannelOpenAL* openal_channel = dynamic_cast<LLAudioChannelOpenAL*>(channel))
        //    {
        //        openal_channel->resumeAfterDeviceReopen();
        //    }
        //}
        mFSResumeChannels.store(true); // FSPerfAudioThread: done by changeDevice() or idle()
        // </FS:Trish>
    }
    else
    {
        LL_WARNS() << "LLAudioEngine_OpenAL::setDevice() error: Reopen failed" << LL_ENDL;
    }
}

LLAudioEngine_OpenAL::output_device_map_t LLAudioEngine_OpenAL::getDevices()
{
    if (!mHasEnumerateAllExt)
        return{};

    const ALCchar* deviceList = alcGetString(nullptr, ALC_ALL_DEVICES_SPECIFIER);

    output_device_map_t devices;
    const ALCchar* nextDevice = deviceList;

    // Parse double-null terminated string block
    while (nextDevice && *nextDevice != '\0')
    {
        std::string device(nextDevice);
        devices.emplace(generate_uuid_from_string(device), device);
        nextDevice += strlen(nextDevice) + 1;
    }

    return devices;
}

void LLAudioEngine_OpenAL::setDevice(const LLUUID& device_uuid)
{
    mSelectedDeviceUUID = device_uuid;
    changeDevice();
}
