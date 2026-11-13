# Ready-made packages

> **On the `dev-untested` branch** this folder is the same as on `main`: the package below is `main`'s tested
> 0.1.0.64. This branch builds Internet 0.1.0.72, which has no ready-made package and has not been run on any device.

| File | MD5 | Notes |
|---|---|---|
| `Browser_0.1.0.64_ARM.appx` | `b1bd87878c3d00cabc575ab87bbec302` | "Internet" 0.1.0.64, built 2026-11-11, the last version tested on the phone |

These packages are a **convenience only**: everything in them can be rebuilt from this repository by following
[docs/browser.md](../docs/browser.md#rebuild-the-app-package-yourself).

**0.1.0.64 is not an exact build of this branch**: it is older code (this branch is 0.1.0.72, see the README for
what it adds), its `webkit.dll` was optimised with a PGO profile
collected on our phone (not published), and its ANGLE DLLs are the prebuilt ones from the MetroSurf 2.0.1 package.
Details in [docs/browser.md](../docs/browser.md#ready-made-package-for-convenience).

Install on a Windows Phone 8.1 (ARM) that accepts unsigned packages (ours is interop-unlocked; a phone that is only
developer-unlocked may refuse it: then rebuild it signed with your own certificate), with `Microsoft.VCLibs.ARM.12.00.Phone.appx` from the
Windows Phone 8.1 SDK installed once:

    AppDeployCmd /install Browser_0.1.0.64_ARM.appx /targetdevice:de

The package is **not signed** (publisher `CN=DumbPhone`).
