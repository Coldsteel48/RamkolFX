# TODO (owner)

Things only the project owner can do: they need accounts, identity or a
decision.

## Accounts and credentials

Every account and key the publishing channels need, in the order you'll
need them. Nothing here goes in the repo: private keys and tokens become
GitHub secrets (`gh secret set ...`) or stay on your machine.

### Keys to generate once

| Key | How | Used by |
|-----|-----|---------|
| SSH key for the AUR | `ssh-keygen -t ed25519 -f ~/.ssh/aur -C aur-ramkolfx` | AUR (public half on the AUR account, private half as secret `AUR_SSH_PRIVATE_KEY`) |
| OpenPGP (GPG) key | `gpg --full-generate-key`, then upload the public key at <https://keys.openpgp.org/upload> | Signing Debian uploads (mentors.debian.net), signed commits in Gentoo's GURU |

### Now: channels CI publishes to on each stable tag

| Users install with | Account to create | Credential | Then |
|--------------------|-------------------|------------|------|
| `pacman` via `yay`/`paru` (Arch, CachyOS, Manjaro) | AUR: <https://aur.archlinux.org/register> | The AUR SSH key above: paste the `.pub` into *My Account → SSH Public Key* | `gh secret set AUR_SSH_PRIVATE_KEY < ~/.ssh/aur` |
| `dnf` (Fedora) | Fedora account: <https://accounts.fedoraproject.org/>, then log in to COPR at <https://copr.fedorainfracloud.org/> and create project `ramkolfx` | COPR API token: <https://copr.fedorainfracloud.org/api/> (**expires every 180 days**) | Save it as `~/.config/copr`, then `gh secret set COPR_CONFIG < ~/.config/copr` |

### Later: official distro repositories

| Users install with | Accounts | Credential |
|--------------------|----------|------------|
| `apt` (Debian) | mentors.debian.net: <https://mentors.debian.net/accounts/register/> (guide: <https://mentors.debian.net/intro-maintainers/>). Salsa, Debian's GitLab, for the packaging repo: <https://salsa.debian.org/users/sign_up> (sign-ups are reviewed by hand) | The OpenPGP key, added to your mentors profile. The ITP needs no account: it's an e-mail to Debian's bug tracker (<https://wiki.debian.org/ITP>) |
| `apt` (Ubuntu) | None: Ubuntu copies the package from Debian | None |
| `dnf` (Fedora official) | The same Fedora account, plus Red Hat Bugzilla for the package review: <https://bugzilla.redhat.com/createaccount.cgi>. Process: <https://docs.fedoraproject.org/en-US/package-maintainers/Joining_the_Package_Maintainers/> | An SSH key on the Fedora account, once a sponsor lets you push to Fedora's package repos |
| `pacman` (Arch official) | None to apply for: an Arch package maintainer adopts packages from the AUR | None |
| `emerge` (Gentoo, GURU repo) | Gentoo Bugzilla: <https://bugs.gentoo.org/createaccount.cgi>. GURU access and rules: <https://wiki.gentoo.org/wiki/Project:GURU> | The OpenPGP key, for signing commits |
| `apk` (Alpine) | Alpine GitLab, for merge requests to aports: <https://gitlab.alpinelinux.org/> | None beyond the account |
| `nix` (NixOS / nixpkgs) | None: a pull request to <https://github.com/NixOS/nixpkgs> with your GitHub account | None |

### Future: app stores

| Store | Account | Credential |
|-------|---------|------------|
| Flathub (Flatpak) | Your GitHub account; submission process: <https://docs.flathub.org/docs/for-app-authors/submission> | None |
| Snap Store | Ubuntu One: <https://login.ubuntu.com/>, then register the name `ramkolfx` at <https://snapcraft.io/> | For CI: a token from `snapcraft export-login` |

## Distro repositories you run (see packaging/PUBLISHING.md)

- [ ] @TODO AUR: create the account and key above, then set the
      `AUR_SSH_PRIVATE_KEY` secret.
- [ ] @TODO COPR: create the Fedora account and the COPR project `ramkolfx`
      (Fedora 42+ chroots, x86_64 and aarch64), then set the `COPR_CONFIG`
      secret. Renew the API token every 180 days.
- [ ] @TODO Tag the next stable release (0.2.3 or later). With the secrets
      set, CI creates the AUR package and starts the first COPR build; 0.2.2
      can't be published there, as its sources predate the packaging changes.
- [ ] @TODO After both are live: add an "Install from your distro" section to
      README.md (or ask Claude to).

## Official distro repositories

- [ ] @TODO Use a real e-mail address as package maintainer. Debian, Fedora
      and Arch all require one that reaches you; today the recipes say
      `noreply@github.com` in packaging/debian/control and in the Debian and
      Fedora changelog entries version.sh writes (`maint=` in
      packaging/version.sh), and the PKGBUILD's `# Maintainer:` line has no
      address at all. Decide which address you're happy to publish.
- [ ] @TODO Decide where the Auto-detect model goes in Debian. Debian's ML
      policy may not accept pretrained weights it can't rebuild in `main`:
      ship it in a separate `ramkolfx-model` package in `contrib`, or have
      the daemon download it on first use. The build already supports
      leaving it out (`-DRAMKOLFX_INSTALL_MODEL_DATA=OFF`); Auto-detect then
      reports itself unavailable.
- [ ] @TODO Debian: file an ITP bug (`reportbug wnpp`, "ITP: ramkolfx"), upload
      to mentors.debian.net, then ask for a sponsor (debian-mentors list or
      the Debian Multimedia team, which looks after audio packages).
      Ubuntu picks the package up from Debian automatically.
- [ ] @TODO Fedora: become a packager (needs a sponsor) and open a package
      review request on bugzilla.redhat.com. KissFFT isn't in Fedora, so
      either package it first or justify the bundled copy in the review.
- [ ] @TODO Arch [extra]: can't be applied for. Once the AUR package has a
      following, an Arch package maintainer may adopt it.
- [ ] @TODO nixpkgs: open a pull request adding the package (Claude can draft
      the derivation from flake.nix).

## Future: app stores

Not started. Both need one code change first: in a sandbox there's no
systemd user service, so the GUI has to start `ramkolfxd` itself when it
isn't running (and keep it running after the window closes), with login
start through the sandbox's background permission instead of the autostart
file.

- [ ] @TODO Flathub: the best fit (EasyEffects, a similar PipeWire effects
      app, is there). Needs the daemon-launch change, the desktop file and
      icon renamed to `io.github.coldsteel48.RamkolFX`, a Flatpak manifest
      that includes ONNX Runtime, then a pull request to Flathub.
- [ ] @TODO Snap Store: check first whether a strictly confined snap may
      create a PipeWire virtual sink. If not, it needs "classic" confinement,
      which Canonical must approve by hand and rarely grants.
- [ ] @TODO AppImage (optional): a single downloadable file, not a store;
      lists on sites like AppImageHub. Also needs the daemon-launch change.
