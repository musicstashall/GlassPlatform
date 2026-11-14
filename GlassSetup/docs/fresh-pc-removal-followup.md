# Fresh source / removal / UI follow-up

## Evidence supplied

- `GlassSetup (2)-2.log`: Setup 2.0.0.276, cached controller child; inherited
  catalog is empty (`Parent catalog check was unavailable`), attached MSI files
  absent, independent OG discovery succeeds. No catalog recovery was attempted.
  This explains why only OG was selectable; missing local packages alone are not
  a product-selection error when a remote catalog is available.
- `GlassSetup (3).log`: GitHub timeout 12002, all five selected removals confirmed;
  original SUT removal returned 3010. Existing profiles WsiAccount and leoka
  were explicitly skipped because their HKU keys were not loaded. Final Setup
  message said maintenance cache was left unchanged.
- MSI logs are removal evidence, not a recursive inventory of Packages. The
  reported 330 MB cannot be assigned file by file without that inventory.

## Fixes

1. Red selected-apply warning for published OG 3.0.2.3749 on Windows build 26200,
   alongside existing 26300 warning. Removal does not get a red danger surface.
   No silent substitution of CI binaries, no new hard compatibility ban.
2. Setup now calls source GC after a successful removal plan, including SUT-only
   and mixed OG removal. Unins also runs GC when a reboot was requested.
   Current/Retained/Previous/Staging MSI sources are checked under the cache lock.
   Exact MSI registration references retain installed sources; uncertain identity,
   ACL/reparse or registration discovery retains the file and reports incomplete
   collection. Reboot pins do not retain an unreferenced *source* MSI. Controllers,
   recovery Work snapshots, Windows Installer's cache, and user download folders
   are not deleted. The design deliberately does not promise an empty shared
   Packages directory or delete the currently executing controller.
3. Valid inherited catalogs still incur zero duplicate network requests. Empty or
   malformed inherited snapshots allow one fetch in the receiving process; the
   mutex shares the result with inspection/acquisition. Another failure does not
   loop or bypass package verification. Offline local sources remain usable.
4. All three panel tops move from 156 to 170; bottoms stay fixed (height 358).
   Lists shrink accordingly, preserving footer space and aligned panel tops.
5. Kept installed products retain their warning button without warning-driven
   yellow card styling. Actual repair/update states remain independent. Available
   cards retain their warning styling.
6. SUT style-removal preflight now inventories all local profiles with existing
   validated ProfileList paths and private RegLoadAppKey handles. Offline profiles
   are pinned/rechecked, not silently skipped. Unreadable/unsafe hives fail before
   vendor removal. Only stock theme selectors are written; files, wallpaper and
   other personalization are not deleted. Live-session switching still belongs
   to the Native helper. Added per-profile inclusion diagnostics.
7. No redundant “ещё / more” footer caption on Available.

## Upstream findings (not a blanket incompatibility statement)

- https://github.com/ALTaleX531/OpenGlass/wiki/Compatibility-and-DWM-architectures
  lists 25H2/26200 as supported but requires exact OS build/revision; its table is
  not a guarantee for every older published binary.
- https://github.com/ALTaleX531/OpenGlass/wiki/ identifies parts of current wiki
  as describing upcoming, unreleased 3.1.0.
- https://github.com/ALTaleX531/OpenGlass/issues/362 reports 25H2 DWM crashes;
  ALTaleX531 requests a dump, not a universal 25H2 prohibition.
- https://github.com/ALTaleX531/OpenGlass/issues/387 concerns 26200.9974 symbols;
  later comments report symbols were added and operation restored. Do not describe
  this as an ongoing universal Microsoft symbol outage.
- https://github.com/ALTaleX531/OpenGlass/releases/tag/v3.0.2.3749 explicitly warns
  about 26300.9818. The additional 26200 warning is a conservative warning for the
  currently offered release and reported failures, not that release note's text.

## Validation / remaining Windows acceptance

Portable tests cover risk version/build boundaries, inherited catalog recovery,
source reference GC despite a reboot pin, kept/unknown source retention, offline
profile inclusion, source GC wiring, aligned panel geometry, neutral kept warning
cards, and both empty overflow captions. No native Windows build was performed.

On Windows: fresh launch after a parent catalog failure, current binary build and
signatures, SUT stock-style preparation in an offline leoka profile, partial/full
removal package inventories, and actual WebView layout remain acceptance checks.

## GlassSetup (4).log: vendor metadata HTTP 403

The first-party catalog succeeds (platform/settings 2.0.0.119, Resources
2.0.0.118). OG and SUT's independent upstream API calls both return HTTP 403.
The log does not establish the specific reason for the 403 response. This is
not the earlier missing first-party catalog problem.

UI now distinguishes `displayVersion` / `versionFallback` from actual
`targetVersion`, `targetPayloadVersion` and `remoteVersion`. With no vendor offer,
OG displays reviewed published 3.0.2.3749 and SUT displays the existing pinned
OfflineV4 release tag 4.0.0, labeled “известная версия / last known release”. The
source strip does not describe these as current server offers. Details explain
that upstream metadata is unavailable. Live/verified-local discovery overrides
fallback display values. Acquisition, trust, hash, identity, downgrade decisions
and version comparison never consume `displayVersion`.

Known-build risk remains visible with empty upstream version (HTTP failure), but
an actually discovered future version is not blanket-flagged. OG risk styling
also applies to the available card before selection, with full-opacity red
surface and red warning icon. Removal and retained installed products are not
styled as dangerous apply. Portable regression replays both empty vendor offers,
checks OG/SUT versions in available/planned cards and red icon computed color.

This fixes UI information loss; it does not make a forbidden upstream HTTP
request succeed. In particular, OG acquisition still requires verified official
metadata/media. No fake server offer or unverified CI binary was introduced.

## Explicit version retry after HTTP 403

A vendor metadata HTTP 403 now sets a distinct blocked capability on that card.
It is not a package-transfer failure and does not create an install intent.
The card shows the existing centered `.download-retry` Play control and the
upstream error/retry explanation. Click sends `metadata-retry:<vendor>`.
A dedicated cancellable worker calls only that vendor's read-only Describe;
there is no full product rescan, selection reset, package download, install,
controller handoff or change to verification rules. Repeated clicks while busy
are suppressed. Completion updates the corresponding offer under inspectMutex
and publishes UI state on the UI thread. Only a successful metadata response
unblocks apply; another failure (including a different HTTP error) stays blocked
and can be retried. Shutdown cancels and joins the worker.

The WebView choice/drag/recommendation capability and native ComponentsReady
all enforce this block. Resources cannot indirectly select a forbidden fresh OG
through the paired-selection path. Product removal is not blocked by inability
to discover a release. Known-version labels and the OG risk warning are display
information only and cannot bypass the metadata gate.

Tests: actual threaded retry/completion with failed/successful responses and
double-click suppression; actual ComponentsReady apply/removal behavior; DOM
reason, Play button, scoped IPC, pending disabled state and successful unblock.
