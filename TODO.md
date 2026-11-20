# TODO (owner)

Things only the project owner can do: they need accounts, identity or a
decision. Code-side work lives in issues, not here.

## Distro repositories you run (see packaging/PUBLISHING.md)

- [ ] @TODO AUR: create an account at aur.archlinux.org, add an SSH key,
      `gh secret set AUR_SSH_PRIVATE_KEY < ~/.ssh/aur`, then do the first
      upload by hand (PUBLISHING.md, "AUR: one-time setup").
- [ ] @TODO COPR: create a Fedora account, a COPR project `ramkolfx` (Fedora
      42+ chroots), `gh secret set COPR_CONFIG < ~/.config/copr`, then start
      the first build from the web UI. Renew the API token every 180 days.
- [ ] @TODO After both are live: add an "Install from your distro" section to
      README.md (or ask Claude to).

## Official distro repositories

- [ ] @TODO Use a real e-mail address as package maintainer. Debian, Fedora
      and Arch all require one that reaches you; today the recipes say
      `noreply@github.com`. It's in packaging/debian/control, the changelog
      entries version.sh writes (`maint=` in packaging/version.sh), the
      Fedora %changelog and the PKGBUILD `# Maintainer:` line. Decide which
      address you're happy to publish.
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
