#!/bin/bash
# The manifests that tools/pro1x-update reads on the phone, made on the PC
# from this repository's GitHub releases (docs/updating.md):
#
#   leste.manifest    the newest version of every package across all
#                     leste-* releases (by Debian version order), with its
#                     release, URL, SHA-256, group and install policy
#   arch.manifest     the same for the arch-* releases (pacman's order),
#                     with each package's depends, provides and conflicts
#                     (format 2: pro1x-update installs a package's
#                     dependencies of the set with it)
#   freebsd.manifest  the newest freebsd-hildon-* release (a pkg repository)
#   leste-files.tar.gz  the repository's script the tool runs for Leste's
#                     mobile-data group (49-mobile-data.sh), at HEAD
#                     (git archive)
#   pro1x-update      the tool itself, at HEAD
#
# Every package is downloaded once (into CACHE) and its SHA-256 checked
# against GitHub's digest of the asset and against the release's SHA256SUMS
# where it has one; name, version and architecture come from the package's
# own control data (.deb) or .PKGINFO (pacman), not from the file name,
# which drops a Debian epoch.
#
#   tools/release/mkmanifest.sh            write them to OUT, publish nothing
#   tools/release/mkmanifest.sh --publish  also upload them to the release
#                                          POINTER (made on the first run, a
#                                          pre-release, never "Latest")
#
#   GH=owner/repo   (default smatkovi/pro1x-maemo-leste)
#   POINTER=tag     (default pro1x-latest)
#   CACHE=/dir      downloaded packages (default $WORK/build/manifest-cache)
#   OUT=/dir        the files made (default $CACHE/out)
#
# Needs gh (logged in for --publish), jq, curl, bsdtar, python3, vercmp
# (pacman), git. The policies below decide what pro1x-update installs; see
# docs/updating.md.
set -euo pipefail
REPO=${REPO:-$(cd "$(dirname "$0")/../.." && pwd)}
WORK=${WORK:-$(cd "$REPO/.." && pwd)}
GH=${GH:-smatkovi/pro1x-maemo-leste}
POINTER=${POINTER:-pro1x-latest}
CACHE=${CACHE:-$WORK/build/manifest-cache}
OUT=${OUT:-$CACHE/out}
PUBLISH=0
case ${1:-} in
--publish) PUBLISH=1 ;;
"") ;;
*) sed -n '2,/^set /p' "$0" | sed '$d'; exit 1 ;;
esac
log() { echo ">> $*"; }
die() { echo "!! $*" >&2; exit 1; }
for t in gh jq curl bsdtar python3 vercmp git sha256sum; do
  command -v "$t" > /dev/null || die "$t missing"
done

# The repository files the tool runs (hooks)
LESTE_FILES="scripts/49-mobile-data.sh rootfs-overlay/etc/init.d/pro1x-modem-wait tools/pro1x-rmnet-flags"
TOOL=tools/pro1x-update/pro1x-update

commit=$(git -C "$REPO" rev-parse HEAD)
# shellcheck disable=SC2086
if [ -n "$(git -C "$REPO" status --porcelain -- $LESTE_FILES $TOOL)" ]; then
  [ "$PUBLISH" = 0 ] || die "uncommitted changes in the files the manifests carry (git archive takes HEAD)"
  log "note: uncommitted changes in the carried files are not in them (HEAD is)"
fi
if [ "$PUBLISH" = 1 ]; then
  git -C "$REPO" fetch -q origin
  [ -n "$(git -C "$REPO" branch -r --contains HEAD)" ] || die "HEAD ($commit) is not pushed"
fi

mkdir -p "$CACHE/pkgs" "$OUT"
URLBASE=https://github.com/$GH/releases/download/$POINTER
NOW=$(date -u +%Y-%m-%dT%H:%M:%SZ)

log "releases of $GH"
# tag, created, asset, size, digest, url
gh api --paginate "repos/$GH/releases?per_page=100" --jq '.[] | select(.draft | not)
  | .tag_name as $t | .created_at as $c | .assets[]
  | [$t, $c, .name, (.size | tostring), (.digest // "-"), .browser_download_url] | @tsv' \
  > "$CACHE/assets.tsv"
[ -s "$CACHE/assets.tsv" ] || die "no release assets"

# A release's SHA256SUMS line for a file, or nothing
sums_of() {
  local tag=$1 name=$2 f=$CACHE/sums/$1
  if [ ! -e "$f" ]; then
    mkdir -p "$CACHE/sums"
    local url
    url=$(awk -F'\t' -v t="$tag" '$1 == t && $3 == "SHA256SUMS" { print $6 }' "$CACHE/assets.tsv")
    if [ -n "$url" ]; then curl -fsSL -o "$f" "$url"; else : > "$f"; fi
  fi
  awk -v n="$name" '{ f = $2; sub(/^\*/, "", f) } f == n { print $1 }' "$f"
}

# Every package (file pattern EXT) of the releases with PREFIX (leste- or
# arch-; pacman's packages may be .xz or .zst): downloaded,
# checked, its control data read. Writes rows of
#   name version arch sha256 size tag created url
# and for pacman's packages into ROWS.meta
#   tag name|depends|provides|conflicts   (each space-separated)
collect() {
  local prefix=$1 ext=$2 rows=$3
  : > "$rows"
  : > "$rows.meta"
  local tag created name size digest url f sha sums meta pn pv pa
  while IFS=$'\t' read -r tag created name size digest url; do
    case $tag in "$prefix"*) ;; *) continue ;; esac
    [ "$tag" != "$POINTER" ] || continue
    # shellcheck disable=SC2254
    case $name in $ext) ;; *) continue ;; esac
    f=$CACHE/pkgs/$tag/$name
    if [ ! -s "$f" ] || [ "$(stat -c %s "$f")" != "$size" ]; then
      mkdir -p "$CACHE/pkgs/$tag"
      curl -fsSL -o "$f.part" "$url"
      mv "$f.part" "$f"
    fi
    sha=$(sha256sum "$f" | cut -d' ' -f1)
    if [ "$digest" != "-" ] && [ "$digest" != "sha256:$sha" ]; then
      die "$tag/$name: SHA-256 $sha, GitHub says $digest"
    fi
    sums=$(sums_of "$tag" "$name")
    if [ -n "$sums" ] && [ "$sums" != "$sha" ]; then
      die "$tag/$name: SHA-256 $sha, its SHA256SUMS says $sums"
    fi
    [ "$digest" != "-" ] || [ -n "$sums" ] || log "note: $tag/$name has neither a digest nor SHA256SUMS"
    if [ "$ext" = '*.deb' ]; then
      meta=$(bsdtar -xOf "$f" 'control.tar*' | bsdtar -xOf - ./control 2> /dev/null ||
        bsdtar -xOf "$f" 'control.tar*' | bsdtar -xOf - control)
      pn=$(printf '%s\n' "$meta" | sed -n 's/^Package: *//p')
      pv=$(printf '%s\n' "$meta" | sed -n 's/^Version: *//p')
      pa=$(printf '%s\n' "$meta" | sed -n 's/^Architecture: *//p')
    else
      meta=$(bsdtar -xOf "$f" .PKGINFO)
      pn=$(printf '%s\n' "$meta" | sed -n 's/^pkgname = //p')
      pv=$(printf '%s\n' "$meta" | sed -n 's/^pkgver = //p')
      pa=$(printf '%s\n' "$meta" | sed -n 's/^arch = //p')
      printf '%s %s|%s|%s|%s\n' "$tag" "$pn" \
        "$(printf '%s\n' "$meta" | sed -n 's/^depend = //p' | tr '\n' ' ')" \
        "$(printf '%s\n' "$meta" | sed -n 's/^provides = //p' | tr '\n' ' ')" \
        "$(printf '%s\n' "$meta" | sed -n 's/^conflict = //p' | tr '\n' ' ')" >> "$rows.meta"
    fi
    [ -n "$pn" ] && [ -n "$pv" ] && [ -n "$pa" ] || die "$tag/$name: no name, version or architecture"
    printf '%s %s %s %s %s %s %s %s\n' "$pn" "$pv" "$pa" "$sha" "$size" "$tag" "$created" "$url" >> "$rows"
  done < "$CACHE/assets.tsv"
}

# The newest of each name and architecture: by version (Debian's order for
# .deb, vercmp for pacman's), the later release where two have the same
# version. Warns where a later release has an older version than an
# earlier one, or the same version with other contents.
newest() {
  local kind=$1 rows=$2
  python3 -I - "$kind" "$rows" <<'PY'
import functools, subprocess, sys

kind, rows = sys.argv[1], sys.argv[2]

def order(c):
    if c.isdigit():
        return 0
    if c.isalpha():
        return ord(c)
    if c == "~":
        return -1
    return ord(c) + 256

def verrevcmp(a, b):
    # dpkg's lib/dpkg/version.c
    i = j = 0
    while i < len(a) or j < len(b):
        first_diff = 0
        while (i < len(a) and not a[i].isdigit()) or (j < len(b) and not b[j].isdigit()):
            ac = order(a[i]) if i < len(a) else 0
            bc = order(b[j]) if j < len(b) else 0
            if ac != bc:
                return ac - bc
            i += 1
            j += 1
        while i < len(a) and a[i] == "0":
            i += 1
        while j < len(b) and b[j] == "0":
            j += 1
        while i < len(a) and a[i].isdigit() and j < len(b) and b[j].isdigit():
            if not first_diff:
                first_diff = ord(a[i]) - ord(b[j])
            i += 1
            j += 1
        if i < len(a) and a[i].isdigit():
            return 1
        if j < len(b) and b[j].isdigit():
            return -1
        if first_diff:
            return first_diff
    return 0

def split(v):
    epoch, _, rest = v.rpartition(":") if ":" in v else ("0", "", v)
    up, _, rev = rest.rpartition("-") if "-" in rest else (rest, "", "")
    return int(epoch or 0), up, rev

def debcmp(a, b):
    ea, ua, ra = split(a)
    eb, ub, rb = split(b)
    if ea != eb:
        return -1 if ea < eb else 1
    return verrevcmp(ua, ub) or verrevcmp(ra, rb)

def paccmp(a, b):
    return int(subprocess.run(["vercmp", a, b], capture_output=True, text=True, check=True).stdout)

cmp = debcmp if kind == "deb" else paccmp
# self-check of the Debian order
for a, b, r in [("1:2.2.196+pro1x5", "1:2.2.196+pro1x4", 1), ("2.24.33-2+pro1x1", "2.24.33-2", 1),
                ("1.0~rc1", "1.0", -1), ("1:0.1", "2.0", 1), ("0.108-1+pro1x4", "0.108-1+pro1x10", -1),
                ("1.4.9+gitc967fbd+pro1x1", "1.4.9", 1), ("2.18.0-1+4m7.1+pro1x1", "2.18.0-1+4m7.1", 1)]:
    got = debcmp(a, b)
    assert (got > 0) - (got < 0) == r, (a, b, got)

by = {}
for line in open(rows):
    name, ver, arch, sha, size, tag, created, url = line.split()
    by.setdefault((name, arch), []).append(dict(ver=ver, sha=sha, size=size, tag=tag, created=created, url=url))
for (name, arch), cands in sorted(by.items()):
    cands.sort(key=lambda c: c["created"])
    for x, y in zip(cands, cands[1:]):
        c = cmp(y["ver"], x["ver"])
        if c < 0:
            print(f"warning: {name} {y['ver']} in {y['tag']} is older than {x['ver']} in the earlier {x['tag']}", file=sys.stderr)
        elif c == 0 and x["sha"] != y["sha"]:
            print(f"warning: {name} {x['ver']} differs between {x['tag']} and {y['tag']} (the later is taken)", file=sys.stderr)
    best = functools.reduce(lambda b, c: c if cmp(c["ver"], b["ver"]) >= 0 else b, cands)
    print(name, best["ver"], arch, best["sha"], best["size"], best["tag"], best["url"])
PY
}

# group: the release's kind (leste-<group>-<date>)
group_of() { local g=${1#*-}; echo "${g%-20[0-9][0-9].*}"; }

# Leste's policies (pro1x-update):
#   always   installed where missing (this repository's own tools, pro1x-*,
#            outside the opt-in groups)
#   installed  only where the package is installed (as 49-hidpi.sh's
#            install_ours: of a source's packages those the system has);
#            --with GROUP installs the group's missing ones too
#   extra    only where installed, also with --with (-dev, -dbgsym, -doc)
#   switch   only where a +pro1x build of it is installed; the first switch
#            from the archive's (with --with GROUP) runs the group's hook
# hold: yes for the rebuilds of Leste's packages (+pro1x in the version),
# as the scripts and release notes hold them
leste_policy() {
  local name=$1 group=$2
  [ "$name" != ofono ] || { echo switch; return; }
  # opt-in: pro1x-waydroid adds Waydroid's apt repository
  case $group in harmattan | waydroid) echo installed; return ;; esac
  case $name in
  pro1x-*) echo always ;;
  *-dev | *-dbgsym | *-doc) echo extra ;;
  *) echo installed ;;
  esac
}

files_tar() {
  local name=$1
  shift
  # shellcheck disable=SC2068
  git -C "$REPO" archive --format=tar HEAD -- $@ | gzip -9n > "$OUT/$name"
}

header() {
  cat << EOF
# pro1x-update manifest, made by tools/release/mkmanifest.sh; docs/updating.md
format ${2:-1}
system $1
generated $NOW
commit $commit
tool $(sha256sum "$OUT/pro1x-update" | cut -d' ' -f1) $URLBASE/pro1x-update
EOF
}

# (a dry run without --publish takes the working tree's, for tests)
if [ "$PUBLISH" = 1 ]; then
  git -C "$REPO" show "HEAD:$TOOL" > "$OUT/pro1x-update"
else
  cp "$REPO/$TOOL" "$OUT/pro1x-update"
fi

# --- Leste ------------------------------------------------------------------
log "Leste: leste-* packages"
collect leste- '*.deb' "$CACHE/leste.rows"
newest deb "$CACHE/leste.rows" > "$CACHE/leste.newest"
files_tar leste-files.tar.gz "$LESTE_FILES"
{
  header leste
  echo "files leste-files.tar.gz $(sha256sum "$OUT/leste-files.tar.gz" | cut -d' ' -f1) $URLBASE/leste-files.tar.gz"
  echo "hook mobile-data leste-files.tar.gz scripts/49-mobile-data.sh"
  echo "# pkg name version arch group policy hold sha256 size release url"
  while read -r name ver arch sha size tag url; do
    group=$(group_of "$tag")
    policy=$(leste_policy "$name" "$group")
    hold=no
    case $ver in *+pro1x*) hold=yes ;; esac
    echo "pkg $name $ver $arch $group $policy $hold $sha $size $tag $url"
  done < "$CACHE/leste.newest"
} > "$OUT/leste.manifest"

# --- Arch -------------------------------------------------------------------
# Groups and policies as on Leste: the core (the parts of the first
# release, arch-hildon-2026.10.30, and this repository's own pro1x-*) is
# installed where missing (the Arch image has these parts from steps 40/41
# without pacman); harmattan (the N9's user interface) and apps (the rest:
# Hildon's apps, their libraries) only where installed or with --with.
# pro1x-update adds what a package it installs depends on in the set
# (dep/prov lines), whatever its group.
ARCH_CORE="libhildon hildon-status-menu hildon-desktop calendar-backend rtcom-eventlogger rtcom-eventlogger-plugins pro1x-events pro1x-volume-osd pro1x-fm"
arch_group() {
  case " $ARCH_CORE " in *" $1 "*) echo core; return ;; esac
  case $1 in
  pro1x-*) echo core ;;
  harmattan-* | qt-components | libngf | ngfd) echo harmattan ;;
  *) echo apps ;;
  esac
}
log "Arch: arch-* packages"
collect arch- '*.pkg.tar.*' "$CACHE/arch.rows"
newest pac "$CACHE/arch.rows" > "$CACHE/arch.newest"
{
  header arch 2
  echo "# pkg name version arch group policy hold sha256 size release url"
  echo "# dep/prov/conflict name list: its .PKGINFO's depend, provides, conflict"
  while read -r name ver arch sha size tag url; do
    group=$(arch_group "$name")
    policy=installed
    [ "$group" != core ] || policy=always
    echo "pkg $name $ver $arch $group $policy no $sha $size $tag $url"
    m=$(awk -v t="$tag" -v n="$name" '$1 == t && substr($0, length(t) + 2, length(n) + 1) == n "|" { print substr($0, length(t) + length(n) + 3); exit }' "$CACHE/arch.rows.meta")
    IFS='|' read -r deps provs confs <<< "$m"
    [ -z "${deps// /}" ] || echo "dep $name $deps"
    [ -z "${provs// /}" ] || echo "prov $name $provs"
    [ -z "${confs// /}" ] || echo "conflict $name $confs"
  done < "$CACHE/arch.newest"
} | sed 's/ *$//' > "$OUT/arch.manifest"

# --- FreeBSD ----------------------------------------------------------------
# The newest freebsd-hildon-* release that is a pkg repository (meta.conf,
# packagesite.pkg); pro1x-update asks GitHub's API itself and takes this
# where that fails
log "FreeBSD: freebsd-hildon-* repositories"
fbsd=$(awk -F'\t' '$1 ~ /^freebsd-hildon-/ && $3 == "packagesite.pkg" { print $1 }' "$CACHE/assets.tsv" |
  sort -u | while read -r t; do
    awk -F'\t' -v t="$t" '$1 == t && $3 == "meta.conf" { found = 1 } END { exit !found }' "$CACHE/assets.tsv" &&
      echo "$t"
  done | sed 's/^freebsd-hildon-//' | sort -t. -k1,1n -k2,2n -k3,3n -k4,4n | tail -1)
[ -n "$fbsd" ] || die "no freebsd-hildon-* repository"
{
  header freebsd
  echo "repo freebsd-hildon-$fbsd https://github.com/$GH/releases/download/freebsd-hildon-$fbsd"
} > "$OUT/freebsd.manifest"

# --- summary ----------------------------------------------------------------
{
  echo "The update manifests that [\`pro1x-update\`](https://github.com/$GH/blob/main/docs/updating.md) reads: one safe update path per system on the F(x)tec Pro1X instead of the separate releases. Not a release of its own: its files are replaced whenever a new release is made (\`tools/release/mkmanifest.sh --publish\`), made at $NOW from commit \`${commit:0:7}\` (the tag stays where it was made)."
  echo
  echo "First run on the phone, as root (dry run; \`--yes\` installs):"
  echo
  echo '    curl -fLo /usr/local/sbin/pro1x-update '"$URLBASE"'/pro1x-update && chmod 755 /usr/local/sbin/pro1x-update'
  echo '    pro1x-update'
  echo
  echo "(FreeBSD: \`fetch -o /usr/local/sbin/pro1x-update $URLBASE/pro1x-update\`.)"
  echo
  echo "### Leste (\`leste.manifest\`)"
  echo
  echo "| package | version | release | policy |"
  echo "|---|---|---|---|"
  awk '$1 == "pkg" { printf "| %s | %s | %s | %s%s |\n", $2, $3, $10, $6, ($7 == "yes" ? ", held" : "") }' "$OUT/leste.manifest"
  echo
  echo "### Arch (\`arch.manifest\`)"
  echo
  echo "| package | version | release | group |"
  echo "|---|---|---|---|"
  awk '$1 == "pkg" { printf "| %s | %s | %s | %s%s |\n", $2, $3, $10, $5, ($6 == "always" ? ", installed where missing" : "") }' "$OUT/arch.manifest"
  echo
  echo "### FreeBSD (\`freebsd.manifest\`)"
  echo
  echo "The pkg repository \`freebsd-hildon-$fbsd\`."
} > "$OUT/notes.md"

ls -l "$OUT"
awk '$1 == "pkg" { print "   " $2, $3, $10, $5, $6, $7 }' "$OUT/leste.manifest" "$OUT/arch.manifest"
echo "   freebsd-hildon-$fbsd"

[ "$PUBLISH" = 1 ] || exit 0
# --- publish ----------------------------------------------------------------
if ! gh release view "$POINTER" -R "$GH" > /dev/null 2>&1; then
  log "release $POINTER (new, at $commit)"
  gh release create "$POINTER" -R "$GH" --target "$commit" --prerelease --latest=false \
    --title "Pro1X update manifests (pro1x-update)" --notes-file "$OUT/notes.md"
else
  gh release edit "$POINTER" -R "$GH" --notes-file "$OUT/notes.md" > /dev/null
fi
# the manifests last: a phone that reads a new manifest finds its files
log "upload to $POINTER"
gh release upload "$POINTER" -R "$GH" --clobber "$OUT/pro1x-update" "$OUT/leste-files.tar.gz"
gh release upload "$POINTER" -R "$GH" --clobber "$OUT/leste.manifest" "$OUT/arch.manifest" "$OUT/freebsd.manifest"
# (the Arch hook's files of format 1, no longer read)
gh release delete-asset "$POINTER" arch-files.tar.gz -R "$GH" -y > /dev/null 2>&1 || true
log "published: https://github.com/$GH/releases/tag/$POINTER"
