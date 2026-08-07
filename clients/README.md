# Clients

The Swift reference client (`CompanionKit`) that used to live at
`clients/swift/CompanionKit` has moved to its own repo:

https://github.com/DarkStarDS9/CompanionKit

It was extracted with full history on 2026-08-07. Both consumer apps
(SpokenFeeds, Snap2Ink) now depend on it via SPM instead of a vendored copy.

**Versioning rule:** the major version tracks the companion-display protocol
version (e.g. `11.0.0` implements protocol v11). Tags cite the firmware
commit whose protocol doc (`docs/companion-display-protocol.md`) they
implement.

There is no other content under `clients/` in this repo. If that changes,
update this file rather than removing it.
