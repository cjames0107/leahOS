#!/usr/bin/env bash
#
# Build the ext4 image that leahOS mounts as its root filesystem, populated from
# the user programs plus a few fixed files the kernel's self-test reads back.
#
#   mkext.sh <out.img> <size-mib> [DEST=SRC ...]
#
# Each DEST=SRC places host file SRC at path DEST inside the image (DEST may name
# a subdirectory, e.g. sbin/init). The feature set is deliberately tamed - no
# metadata_csum, 64bit or dir_index - so the kernel's writer can keep the volume
# e2fsck-clean without computing checksums, 64-bit group descriptors or an HTree
# index. Extents and the journal stay on, so it is a genuine ext4 volume.

set -euo pipefail

if [ "$#" -lt 2 ]; then
    echo "usage: mkext.sh <out.img> <size-mib> [DEST=SRC ...]" >&2
    exit 2
fi

OUT="$1"; SIZE_MIB="$2"; shift 2

# Where mke2fs and debugfs live.
#
# Homebrew keeps e2fsprogs off the default PATH, because macOS has its own
# tools by those names for its own filesystems. Asking `brew --prefix` was the
# obvious way to find them and turned out to be the fragile one: brew refuses
# to run at all when it dislikes the working directory, and under `set -e` a
# refusal it printed to a swallowed stderr took the whole build down with no
# message at all. So the known locations are tried first and brew is the last
# resort rather than the first.
E2_DIR=""
for candidate in \
    /opt/homebrew/opt/e2fsprogs/sbin \
    /usr/local/opt/e2fsprogs/sbin \
    /sbin /usr/sbin
do
    if [ -x "$candidate/mke2fs" ] && [ -x "$candidate/debugfs" ]; then
        E2_DIR="$candidate"
        break
    fi
done
if [ -z "$E2_DIR" ]; then
    prefix="$(brew --prefix e2fsprogs 2>/dev/null || true)"
    if [ -n "$prefix" ] && [ -x "$prefix/sbin/mke2fs" ]; then
        E2_DIR="$prefix/sbin"
    fi
fi
MKE2FS="$E2_DIR/mke2fs"
if [ -z "$E2_DIR" ] || [ ! -x "$MKE2FS" ]; then
    echo "error: mke2fs and debugfs not found" >&2
    echo "install them with:  brew install e2fsprogs" >&2
    exit 1
fi

STAGING="$(mktemp -d)"
trap 'rm -rf "$STAGING"' EXIT

# The Filesystem Hierarchy Standard, as much of it as means anything here.
#
# Every directory the standard calls for is created even when this system has
# nothing to put in it yet, because an empty /srv is a place for the next
# person to put something and a missing one is a decision they have to make
# again. The exceptions are noted where they are made.
mkdir -p "$STAGING/bin" "$STAGING/sbin" "$STAGING/etc" "$STAGING/dev" \
         "$STAGING/boot" "$STAGING/lib" "$STAGING/media" "$STAGING/mnt" \
         "$STAGING/Applications" "$STAGING/opt" "$STAGING/srv" "$STAGING/run" "$STAGING/tmp" \
         "$STAGING/proc" "$STAGING/sys" \
         "$STAGING/usr/bin" "$STAGING/usr/sbin" "$STAGING/usr/lib" \
         "$STAGING/usr/include" "$STAGING/usr/src" "$STAGING/usr/local/bin" \
         "$STAGING/usr/local/lib" "$STAGING/usr/local/share" \
         "$STAGING/usr/share/doc" "$STAGING/usr/share/icons" \
         "$STAGING/usr/share/wallpapers" "$STAGING/usr/share/demos" \
         "$STAGING/usr/share/man" "$STAGING/usr/share/fonts" \
         "$STAGING/var/log" "$STAGING/var/tmp" "$STAGING/var/cache" \
         "$STAGING/var/lib" "$STAGING/var/spool"

# /proc holds what the kernel says about itself, and vfsd answers for all of it
# without touching the disk - the directory here is the mount point and nothing
# else, which is why it stays empty. /sys is still only a name.

# /dev is made further down, with debugfs: a device node cannot be staged as a
# file, and mknod(2) on the build host needs root. The numbers are the ones
# Linux uses, so a node means the same thing on either system.

# Application bundles. A .app is a directory that carries its own description,
# so nothing else needs a built-in table of which program opens what - see
# user/libc/include/bundle.h. They are staged here rather than assembled at
# runtime because a bundle is a filesystem layout, and the image is where the
# filesystem gets laid out.
#
# Stage one application bundle: the directory, its binary, its Info, and an
# icon. Everything an application declares about itself is written here, which
# is the point - there is no second copy of these facts anywhere in the system.
#
#   stage_bundle <Name> <src.elf> <icon> <opens> <menu...>
stage_bundle() {
    local app="$1"; local src="$2"; local icon="$3"; local opens="$4"; shift 4
    local dir="$STAGING/Applications/$app.app"
    local exe
    # Without the .elf. That extension is a fact about the build directory -
    # it is how the Makefile tells a linked binary from an object file - and it
    # has no business being visible in an installed system. Nothing else here
    # carries it: /bin/ls is ls, and Calendar.app should hold Calendar's
    # program, not Calendar's build artefact. The Info file names the
    # executable, so the bundle stays self-describing either way.
    exe="$(basename "$src")"
    exe="${exe%.elf}"
    mkdir -p "$dir"
    cp "$src" "$dir/$exe"
    {
        printf 'name %s\n' "$app"
        printf 'exec %s\n' "$exe"
        printf 'icon Icon.png\n'
        if [ -n "$opens" ]; then printf 'opens %s\n' "$opens"; fi
        local item
        for item in "$@"; do printf 'menu %s\n' "$item"; done
    } > "$dir/Info"
    # A drawn icon when there is one, and a generated tile when there is not.
    # mkicon.py stays for exactly that case: a bundle that claims an icon it
    # does not have is worse than one that claims none.
    if [ -n "$icon" ] && [ -f "media/icons/$icon.png" ]; then
        cp "media/icons/$icon.png" "$dir/Icon.png"
    else
        python3 tools/mkicon.py "$dir/Icon.png" "$app"
    fi
}

printf 'Hello from ext4.\n' > "$STAGING/usr/share/doc/hello.txt"
printf 'Notes live in a subdirectory.\n' > "$STAGING/usr/share/doc/notes.txt"
{
    printf '# leahOS\n\n'
    for _ in $(seq 1 120); do
        printf 'leahOS reads this file back through the ext driver, block by block. '
    done
    printf '\n'
} > "$STAGING/usr/share/doc/readme.md"

# Accounts, home directories and the shadow file.
python3 "$(dirname "$0")/mkaccounts.py" "$STAGING"

# Requested files (the user programs).
for pair in "$@"; do
    dest="${pair%%=*}"
    src="${pair#*=}"
    mkdir -p "$STAGING/$(dirname "$dest")"
    cp "$src" "$STAGING/$dest"
    # The execute bits are how the system tells a program from a document now
    # that there is no .ELF on the end to look at.
    chmod 755 "$STAGING/$dest"
done

# Every application, as a complete bundle. The binaries come straight from the
# build rather than from the staged command directories, because these
# deliberately do not go on the command path at all - see the Makefile.
#
# The table is here rather than in the Makefile because everything on a line is
# a fact about the application: what it is called, what it opens, and what it
# offers when right-clicked.
stage_app() {                # <Name> <make-name> <icon> <opens> <menu...>
    local name="$1"; local prog="$2"; shift 2
    local src="$BUILD_DIR/$prog.elf"
    if [ -f "$src" ]; then stage_bundle "$name" "$src" "$@"; fi
}
BUILD_DIR="${BUILD_DIR:-build}"
stage_app Files     browse   files      ""              "New window" "New folder"
stage_app Terminal  term     terminal   ""              "New terminal"
stage_app Edit      edit     edit       ".TXT .MD .C .H .LEAHRC" "New document"
stage_app Paint     paint    paint      ".PNG .GIF"     "New drawing"
stage_app Images    imgview  images     ".PNG"          "Open picture..."
stage_app Calculator calc    calculator ""              ""
stage_app Settings  settings settings   ""              "Appearance" "Users"
stage_app Tasks     taskman  tasks      ""              "End task"
stage_app Clock     clock    ""         ""              ""
stage_app Elements  uitest   elements   ""              ""
stage_app Music     player   ""         ".WAV .MP3 .OGG" "Open sound..."
stage_app "Disk Utility"    diskutil ""      ""              "Verify" "Repair"
stage_app "Network Utility" netutil  ""      ""              "Ping" "Look up"
stage_app Calendar  calendar ""         ""              "Today"
stage_app "Resource Monitor" resmon  ""      ""              ""
stage_app Console   console  ""         ""              "Jump to end"
stage_app Web       web      ""         ".HTML .HTM"    "New tab" "Bookmark this page"
stage_app Write     write    edit       ".RTF"          "New document" "Open..."
stage_app Archiver  archiver ""         ".TAR .GZ .TGZ" "Open..." "New Archive..."
stage_app Grab      grab     ""         ""              "Capture"
stage_app Fonts     fonts    ""         ".TTF"          ""
stage_app Help      help     ""         ""              "" 

# The manual. Plain text: troff is a typesetting language, and the reason
# manual pages are written in it is that in 1971 the same source had to drive a
# phototypesetter - not a problem anybody here has. What is left when that goes
# is a file somebody can read with cat, and a formatter nobody has to write.
if [ -d docs/man ]; then
    cp docs/man/*.1 "$STAGING/usr/share/man/"
fi

# The font. One file, already stripped by tools/mkfont.py to the tables a
# rasteriser reads - the four megabytes it arrives as are variable-axis deltas
# and layout tables that nothing here consults.
if [ -n "${FONT_DIR:-}" ] && [ -d "$FONT_DIR" ]; then
    cp "$FONT_DIR"/*.ttf "$STAGING/usr/share/fonts/" 2>/dev/null || true
fi

# The icon set, copied in as-is. These are ordinary compressed PNGs - nothing
# converts them at build time, because img_read_png inflates a real deflate
# stream now and can simply read them.
# The icon set. It arrives sorted into folders now - UI/ for the pieces of the
# file manager, applications/ for the things in the dock, glyphs/ for the
# Material symbols the new chrome draws - and is flattened on the way in,
# because a path is how a program names an icon and it should not have to know
# which drawer the artwork was filed in. Names are lowercased so that Files.png
# and files.png are the same icon, which on a case-sensitive filesystem they
# otherwise are not.
if [ -d media/icons ]; then
    for group in UI applications; do
        [ -d "media/icons/$group" ] || continue
        for icon in "media/icons/$group"/*.png; do
            [ -e "$icon" ] || continue
            base="$(basename "$icon" | tr '"'"'A-Z'"'"' '"'"'a-z'"'"')"
            cp "$icon" "$STAGING/usr/share/icons/$base"
        done
    done
    # The glyphs stay as they are: they are vector, and the chrome fills them
    # at whatever size it needs rather than at one baked in here.
    if [ -d media/icons/glyphs ]; then
        mkdir -p "$STAGING/usr/share/icons/glyphs"
        # Named for what they are, not for how they were exported. The files
        # arrive as close_24dp_E3E3E3_FILL0_wght400_GRAD0_opsz24.svg, which
        # records a size, a colour and four axis values that a vector renderer
        # setting all four itself has no use for.
        for glyph in media/icons/glyphs/*.svg; do
            [ -e "$glyph" ] || continue
            short="$(basename "$glyph" | sed 's/_[0-9]*dp_.*//')"
            cp "$glyph" "$STAGING/usr/share/icons/glyphs/$short.svg"
        done
    fi
fi

# The photographs and the music, which do need converting - JPEG and MP3 are
# decoders this system does not have. tools/mkmedia.py has already turned them
# into PNG and 16-bit PCM under $MEDIA_DIR; here they are only placed.
#
# Wallpapers go beside the icons in /share, because they are part of the system
# rather than anyone's documents. The demos go in /Demos, at the root, where
# somebody opening the file browser will actually find them.
MEDIA_DIR="${MEDIA_DIR:-build/media}"
if [ -d "$MEDIA_DIR/wallpapers" ]; then
    cp "$MEDIA_DIR"/wallpapers/*.png "$STAGING/usr/share/wallpapers/" 2>/dev/null || true
fi
# A gzip file and a tar, so gunzip and tar have something real to be pointed
# at from inside the system rather than only in a test harness.
if [ -d "$MEDIA_DIR/testkit" ]; then
    cp "$MEDIA_DIR"/testkit/* "$STAGING/usr/share/doc/" 2>/dev/null || true
    # A script, executable, so that #! can be tried by name.
    if [ -f "$MEDIA_DIR/testkit/hello.sh" ]; then
        cp "$MEDIA_DIR/testkit/hello.sh" "$STAGING/usr/local/bin/hello.sh"
        chmod 755 "$STAGING/usr/local/bin/hello.sh"
    fi
fi
if [ -d "$MEDIA_DIR/demos" ]; then
    mkdir -p "$STAGING/usr/share/demos/images" "$STAGING/usr/share/demos/audio"
    cp "$MEDIA_DIR"/demos/images/*.png "$STAGING/usr/share/demos/images/" 2>/dev/null || true
    cp "$MEDIA_DIR"/demos/audio/*.wav "$STAGING/usr/share/demos/audio/" 2>/dev/null || true
fi

# A desktop with three things on it, for every account that has one. An empty
# desktop is a correct desktop and a poor first impression: these say what is
# here and give somewhere to start.
#
# A shortcut is a ".alias" file holding the path it stands for - there are no
# symbolic links in this filesystem, and a text file needs neither the kernel
# nor the on-disk format to know anything new. See bundle.h.
stage_desktop() {                   # <home>
    local home="$1"
    # A home is a place to keep things, so it arrives with somewhere to keep
    # them. Public is the only one whose mode differs, and the only one that
    # needs explaining: it is the one other people may look in.
    mkdir -p "$STAGING/$home/Desktop" "$STAGING/$home/Documents" \
             "$STAGING/$home/Apps" "$STAGING/$home/Public"
    printf '/Applications/Files.app\n' > "$STAGING/$home/Desktop/Files.alias"
    printf '/Applications/Edit.app\n'  > "$STAGING/$home/Desktop/Notepad.alias"
    printf '/usr/share/doc/readme.md\n' > "$STAGING/$home/Desktop/Readme.alias"
}

# Something for the Archiver to open, and something for Write.
#
# Both applications are perfectly capable of starting empty; both are much
# easier to believe when there is a file to point them at, and a system that
# ships an archive tool with nothing to unpack is asking to be taken on trust.
stage_samples() {                   # <home>
    local home="$1"
    if command -v tar >/dev/null 2>&1; then
        # COPYFILE_DISABLE and --format ustar because the host may be macOS,
        # whose tar otherwise writes an AppleDouble "._name" beside every
        # member and a PaxHeader in front of it. The archive still reads, and
        # it reads as three times as many files as it has.
        COPYFILE_DISABLE=1 tar --format ustar \
            -cf "$STAGING/$home/Documents/manual.tar" -C docs man 2>/dev/null || true
    fi
    cat > "$STAGING/$home/Documents/welcome.rtf" <<'RTF'
{\rtf1\ansi\deff0{\fonttbl{\f0\fswiss Helvetica;}}
\ql\fs36 Welcome\par
\fs24 This is a rich text document. The words above are larger than these; \b this part is bold\b0 , \i this part leans\i0 , and \ul this part is underlined\ulnone .\par
\par
It was written by hand rather than by the editor, which is the point: the file is the format, and anything that reads RTF can read it.\par
}
RTF
}
stage_desktop root
stage_desktop home/leah
stage_desktop home/guest
# After the desktops, because that is what makes the Documents folder these go
# into. Before it, both files were written into a directory that did not exist
# yet and neither of them arrived.
stage_samples root
stage_samples home/leah

dd if=/dev/zero of="$OUT" bs=1048576 count="$SIZE_MIB" status=none

# The feature set / block / inode size can be overridden to build read-only
# breadth images that exercise the harder reader paths (64bit descriptors,
# HTree directories, metadata checksums, 1 KiB blocks). The default is the tamed
# set the kernel's writer targets.
MKE2FS_FEATURES="${MKE2FS_FEATURES:-^metadata_csum,^64bit,^dir_index,^orphan_file}"
MKE2FS_BLOCK="${MKE2FS_BLOCK:-4096}"
MKE2FS_INODE="${MKE2FS_INODE:-256}"

# -F: operate on a plain file. -d: populate from the staging tree at creation.
# Labelled, because the filesystem server finds its root by looking for this
# rather than by being told which disk to trust. Two disks can both be ext4;
# only one of them is the system.
"$MKE2FS" -q -F -t ext4 -b "$MKE2FS_BLOCK" -I "$MKE2FS_INODE" \
    -O "$MKE2FS_FEATURES" -L leahroot \
    -d "$STAGING" "$OUT" >/dev/null

# mke2fs -d copies the *host* ownership, which is whoever built the image - so
# fix up the things whose owner is part of the point. debugfs writes inode
# fields directly; sif is "set inode field".
DEBUGFS="$E2_DIR/debugfs"
if [ -x "$DEBUGFS" ]; then
    {
        # The shadow file must be unreadable to anyone but root. Nothing in
        # userland reads it anyway - the kernel does - but a world-readable
        # hash file would undo the point of hashing.
        echo "sif /etc/shadow uid 0"
        echo "sif /etc/shadow gid 0"
        echo "sif /etc/shadow mode 0100600"
        echo "sif /etc/passwd mode 0100644"
        # Home directories are private. mke2fs -d copies the *host* mode as
        # well as the host owner, which leaves them world readable - and a home
        # directory anyone can read makes the whole account model decorative.
        echo "sif /root uid 0"
        echo "sif /root gid 0"
        echo "sif /root mode 040700"
        # root's folders and desktop are root's too - mke2fs -d gave them to
        # whoever built the image.
        for d in Desktop Documents Apps Public; do
            echo "sif /root/$d uid 0"
            echo "sif /root/$d gid 0"
        done
        for f in Files.alias Notepad.alias Readme.alias; do
            echo "sif /root/Desktop/$f uid 0"
            echo "sif /root/Desktop/$f gid 0"
        done
        echo "sif /root/Public mode 040755"
        echo "sif /home/leah uid 1000"
        echo "sif /home/leah gid 1000"
        echo "sif /home/leah mode 040700"
        # The desktop belongs to the account too - mke2fs -d copies the host's
        # ownership, so without this a user's own Desktop is owned by whoever
        # built the image and is read-only to them inside their own home.
        for f in "" /Files.alias /Notepad.alias /Readme.alias; do
            echo "sif /home/leah/Desktop$f uid 1000"
            echo "sif /home/leah/Desktop$f gid 1000"
        done
        # The folders a home arrives with belong to the account too. Public is
        # 0755 on purpose - it is the one that is meant to be readable.
        for d in Documents Apps Public; do
            echo "sif /home/leah/$d uid 1000"
            echo "sif /home/leah/$d gid 1000"
        done
        echo "sif /home/leah/Documents mode 040700"
        echo "sif /home/leah/Apps mode 040700"
        echo "sif /home/leah/Public mode 040755"
        echo "sif /home/leah/Desktop mode 040700"
        echo "sif /home/guest uid 1001"
        echo "sif /home/guest gid 1001"
        echo "sif /home/guest mode 040700"
        for f in "" /Files.alias /Notepad.alias /Readme.alias; do
            echo "sif /home/guest/Desktop$f uid 1001"
            echo "sif /home/guest/Desktop$f gid 1001"
        done
        # The folders a home arrives with belong to the account too. Public is
        # 0755 on purpose - it is the one that is meant to be readable.
        for d in Documents Apps Public; do
            echo "sif /home/guest/$d uid 1001"
            echo "sif /home/guest/$d gid 1001"
        done
        echo "sif /home/guest/Documents mode 040700"
        echo "sif /home/guest/Apps mode 040700"
        echo "sif /home/guest/Public mode 040755"
        echo "sif /home/guest/Desktop mode 040700"

        # The device nodes. Character devices all: nothing here is a disk.
        #
        # cd first, and bare names after it. Given an absolute path debugfs
        # allocates the inode and then does not link it to anything, which
        # leaves /dev empty and the inode lost - it reports success either way,
        # so this is worth stating rather than discovering twice.
        echo "cd /dev"
        echo "mknod null c 1 3"
        echo "mknod zero c 1 5"
        echo "mknod full c 1 7"
        echo "mknod tty c 5 0"
        echo "mknod console c 5 1"
        # mknod leaves the mode at the type bits alone, so every one of these
        # would be unreadable and unwritable by anybody without this.
        for node in null zero full tty; do
            echo "sif $node mode 020666"
        done
        # The console is the machine's own, not a terminal a user was given.
        echo "sif console mode 020600"
    } | "$DEBUGFS" -w "$OUT" >/dev/null 2>&1
fi

echo "ext4:   $OUT ($SIZE_MIB MiB, block $MKE2FS_BLOCK), populated from $(($#)) files + fixtures"
