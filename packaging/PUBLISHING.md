# Publishing to distro repositories

After a **stable** tag (`vX.Y.Z`, no `-alpha`/`-beta`/`-rc`) passes the
`Packages` workflow, two extra jobs publish it:

| Job    | Publishes to | Users install with | Needs |
|--------|--------------|--------------------|-------|
| `aur`  | AUR package `ramkolfx` | `yay -S ramkolfx` / `paru -S ramkolfx` | secret `AUR_SSH_PRIVATE_KEY` |
| `copr` | COPR project `ramkolfx` | `sudo dnf copr enable <fas-user>/ramkolfx && sudo dnf install ramkolfx` | secret `COPR_CONFIG` |

Until its secret exists, each job only prints a notice and succeeds.
Pre-releases are never published: on Arch, `vercmp` sorts `0.3.0_alpha1`
*after* `0.3.0`, so an alpha in the AUR would block the final release.

## AUR: one-time setup

1. Create an account at <https://aur.archlinux.org/register>.
2. Make a key used only for the AUR, and add its **public** half under
   *My Account → SSH Public Key*:

   ```sh
   ssh-keygen -t ed25519 -f ~/.ssh/aur -C aur-ramkolfx -N ''
   cat ~/.ssh/aur.pub
   ```

3. Give CI the **private** half:

   ```sh
   gh secret set AUR_SSH_PRIVATE_KEY < ~/.ssh/aur
   ```

The first stable tag after this creates the package; no manual upload is
needed. (Don't publish 0.2.2 or older by hand: those sources predate the
`RAMKOLFX_SYSTEM_*` options the PKGBUILD now passes, so they won't build.)
To try the AUR package locally once that tag is pushed:

```sh
mkdir /tmp/aur-ramkolfx && packaging/aur/update.sh /tmp/aur-ramkolfx
cd /tmp/aur-ramkolfx && makepkg -si
```

`packaging/aur/update.sh` copies `packaging/arch/PKGBUILD` and fills in the
real checksum of GitHub's tag tarball (the in-repo copy has `SKIP`, because
`version.sh sync` runs before the tag exists). It also writes `.SRCINFO`.
To change the package, edit `packaging/arch/PKGBUILD`, never the AUR copy.
If you release a packaging-only fix without a new version, bump `pkgrel` by hand.

## COPR: one-time setup

1. Create a Fedora account at <https://accounts.fedoraproject.org> and log in
   at <https://copr.fedorainfracloud.org>.
2. *New Project* → name `ramkolfx`. Under chroots, pick the supported
   **Fedora** releases (42 or newer: the daemon needs PipeWire ≥ 1.4) for
   `x86_64` and `aarch64`. EPEL lacks PipeWire 1.4, so leave it out.
3. Copy the API config from <https://copr.fedorainfracloud.org/api/> and
   give it to CI:

   ```sh
   gh secret set COPR_CONFIG < ~/.config/copr   # or paste the text
   ```

   The token **expires after 180 days**. When the `copr` job starts failing
   with an auth error, renew it there and set the secret again.
4. If the project isn't named `ramkolfx`, set the repo variable instead:
   `gh variable set COPR_PROJECT --body <name>`.

The first stable tag after this starts the first build. (Releases up to
0.2.2 can't be built there: they lack `.copr/` and the `RAMKOLFX_SYSTEM_*`
options the spec now passes.)

COPR runs [`.copr/Makefile`](../.copr/Makefile) to download the spec's
sources and make the SRPM, then builds that offline for each chroot.
