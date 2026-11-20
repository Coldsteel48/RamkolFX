#!/bin/sh
# Runs inside an archlinux container: publish the PKGBUILD for ./VERSION to the
# AUR package "ramkolfx". Repo checkout at /src; the private half of an SSH key
# registered on the AUR account in $AUR_SSH_PRIVATE_KEY.
set -eu
: "${AUR_SSH_PRIVATE_KEY:?not set}"
pacman -Syu --noconfirm --needed git openssh curl
useradd -m builder   # makepkg (for .SRCINFO) refuses to run as root

ssh=/home/builder/.ssh
install -d -m 700 "$ssh"
printf '%s\n' "$AUR_SSH_PRIVATE_KEY" > "$ssh/aur"
chmod 600 "$ssh/aur"
cat > "$ssh/config" <<'EOF'
Host aur.archlinux.org
  User aur
  IdentityFile ~/.ssh/aur
  IdentitiesOnly yes
EOF
ssh-keyscan -t ed25519 aur.archlinux.org > "$ssh/known_hosts"
chown -R builder "$ssh"

V=$(tr -d '[:space:]' < /src/VERSION)
su builder -s /bin/sh -c "
set -eu
cd ~
git clone ssh://aur@aur.archlinux.org/ramkolfx.git aur
sh /src/packaging/aur/update.sh aur
cd aur
git add PKGBUILD .SRCINFO
if git diff --cached --quiet; then
    echo 'AUR already has $V'
    exit 0
fi
git -c user.name=Coldsteel48 -c user.email=noreply@github.com commit -q -m 'Update to $V'
git push origin HEAD:master
"
