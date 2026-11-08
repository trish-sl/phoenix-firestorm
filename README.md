<picture>
  <source media="(prefers-color-scheme: dark)" srcset="doc/trishstorm_mark_dark.png">
  <img align="left" width="100" height="100" src="doc/trishstorm_mark_light.png" alt="TrishStorm logo">
</picture>

**TrishStorm is a personal development fork of the [Firestorm](https://github.com/FirestormViewer/phoenix-firestorm) viewer for Second Life.**

<br clear="left">

> [!WARNING]
> **TrishStorm is not a released viewer, and there are no downloads.** It is a dev test viewer. Branches here can contain experimental, unfinished or broken code, and history is frequently rewritten. Do not use it as your daily viewer, and please don't ask the Firestorm support team for help with it.

## What this fork is for

TrishStorm is where I (TrishAce) develop and test fixes and improvements before upstreaming to the official [Second Life viewer](https://github.com/secondlife/viewer) and to [Firestorm](https://github.com/FirestormViewer/phoenix-firestorm), where they fit. Work here includes:

- bug fixes for issues found in the upstream viewers;
- rendering and performance work: shadows, lighting, post effects, culling, and moving work off the main thread;
- other experiments that need testing in-world before they are proposed upstream.

Most new features are behind settings and are off by default. Changes that are ready are offered back as pull requests to the upstream projects, following their own contribution rules. Anything that isn't reeady for upstream stays here as an experiment. No-submitted code is often left in a "dirty" state during testing, there's no guarantee that the code you are looking at is proper or functional.

## Relationship to Firestorm and Second Life

TrishStorm is based on Firestorm and aims to stay relatively on par with it, it is a third party viewer derived from the official Second Life viewer. All credit for those projects goes to The Phoenix Firestorm Project, Inc., Linden Lab, and their contributors. TrishStorm is not affiliated with or endorsed by either of them.

The code is available under the LGPL, like the projects it derives from. See [LICENSE](LICENSE) and the license headers in each file.

Use of this code in-world follows the Second Life [Third Party Viewer Policy](https://secondlife.com/corporate/third-party-viewers) and the [Second Life Code of Conduct](https://github.com/secondlife/viewer?tab=coc-ov-file).

## Building

The build process is the same as Firestorm's:

- [Windows](doc/building_windows.md)
- [Mac](doc/building_macos.md)
- [Linux](doc/building_linux.md)

> [!NOTE]
> Self-compiled builds of this fork are unsupported. For Firestorm itself, use the official builds from the [Firestorm website](https://www.firestormviewer.org/choose-your-platform/).

## Upstream projects

- **Firestorm:** [source](https://github.com/FirestormViewer/phoenix-firestorm), [bug tracker](https://jira.firestormviewer.org), [contributing guide](CONTRIBUTING.md)
- **Second Life viewer:** [source](https://github.com/secondlife/viewer)
