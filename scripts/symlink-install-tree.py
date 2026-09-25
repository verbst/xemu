#!/usr/bin/env python3

from pathlib import PurePath
import errno
import json
import os
import shlex
import shutil
import subprocess
import sys

# Windows refuses to create a symbolic link unless the account holds
# SeCreateSymbolicLinkPrivilege, which ordinarily means Developer Mode or an
# elevated shell.
ERROR_PRIVILEGE_NOT_HELD = 1314

def destdir_join(d1: str, d2: str) -> str:
    if not d1:
        return d2
    # c:\destdir + c:\prefix must produce c:\destdir\prefix
    return str(PurePath(d1, *PurePath(d2).parts[1:]))

introspect = os.environ.get('MESONINTROSPECT')
out = subprocess.run([*shlex.split(introspect), '--installed'],
                     stdout=subprocess.PIPE, check=True).stdout
def symlinks_forbidden(e):
    """True when Windows refused the link for want of privilege."""
    return (os.name == 'nt' and isinstance(e, OSError)
            and getattr(e, 'winerror', None) == ERROR_PRIVILEGE_NOT_HELD)


for source, dest in json.loads(out).items():
    bundle_dest = destdir_join('qemu-bundle', dest)
    path = os.path.dirname(bundle_dest)
    try:
        os.makedirs(path, exist_ok=True)
    except BaseException as e:
        print(f'error making directory {path}', file=sys.stderr)
        raise e
    try:
        os.symlink(source, bundle_dest)
    except BaseException as e:
        if isinstance(e, OSError) and e.errno == errno.EEXIST:
            continue
        if symlinks_forbidden(e):
            # This tree only exists so the binary can find its data files when
            # run from the build directory without being installed. At runtime
            # its absence is checked for and handled, so a build without it is
            # still a working build -- but a half-built one is not, because the
            # check is for the directory rather than its contents. Remove what
            # was created so the fallback is reached cleanly.
            print('Not creating the uninstalled-run symlink tree: Windows '
                  'requires Developer Mode or an elevated shell to make '
                  'symbolic links.', file=sys.stderr)
            print('This only affects running from the build directory; an '
                  'installed or packaged build is unaffected.', file=sys.stderr)
            shutil.rmtree('qemu-bundle', ignore_errors=True)
            sys.exit(0)
        print(f'error making symbolic link {dest}', file=sys.stderr)
        raise e
