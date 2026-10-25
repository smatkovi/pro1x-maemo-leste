#!/usr/bin/python3
"""Leste applications in the MeeGo session.

The MeeGo boot runs Harmattan's user interface on Leste, and Leste's own
applications still work on the same X display: the camera (pro1x-camera on
libcamera), Firefox ESR, gPodder... This puts them into Harmattan's
application grid:

  leste-apps.py sync <harmattan tree>
      writes a Harmattan desktop entry (and icon) for every application
      listed in /etc/pro1x-meego/leste-apps (one Leste desktop id per line,
      from /usr/share/applications or its hildon/ directory); entries of
      applications no longer listed are removed
  leste-apps.py serve
      the system bus service org.pro1x.LesteLauncher, whose Launch(s id)
      the entries call: it starts the listed application as Leste's user
      on the display, with Leste's session bus and PulseAudio

Runs as root on Leste (40-harmattan-session.sh). Only listed applications
can be started, by their desktop entry's Exec line.
"""
import configparser
import glob
import os
import shlex
import shutil
import subprocess
import sys

LIST = '/etc/pro1x-meego/leste-apps'
DESKTOP_DIRS = ('/usr/share/applications/hildon', '/usr/share/applications')
PREFIX = 'pro1x-leste-'
NAME = 'org.pro1x.LesteLauncher'
PATH = '/org/pro1x/LesteLauncher'
USER = 'user'


def listed():
    try:
        with open(LIST) as f:
            return [l.strip() for l in f if l.strip() and not l.startswith('#')]
    except OSError:
        return []


def desktop_entry(app):
    for d in DESKTOP_DIRS:
        path = os.path.join(d, app + '.desktop')
        if os.path.exists(path):
            cfg = configparser.ConfigParser(interpolation=None, strict=False)
            cfg.optionxform = str
            cfg.read(path, encoding='utf-8')
            if cfg.has_section('Desktop Entry'):
                return cfg['Desktop Entry']
    return None


def find_icon(name):
    if not name:
        return None
    if os.path.isabs(name) and os.path.exists(name):
        return name
    best, best_size = None, -1
    for path in glob.glob('/usr/share/icons/**/%s.png' % name, recursive=True) + \
            glob.glob('/usr/share/pixmaps/%s.png' % name):
        size = 0
        for part in path.split('/'):
            if 'x' in part and part.split('x')[0].isdigit():
                size = int(part.split('x')[0])
        if size > best_size:
            best, best_size = path, size
    return best


def sync(harmattan):
    apps_dir = os.path.join(harmattan, 'usr/share/applications')
    icon_dir = os.path.join(harmattan, 'usr/share/pixmaps/pro1x-leste')
    os.makedirs(icon_dir, exist_ok=True)
    wanted = set()
    for app in listed():
        entry = desktop_entry(app)
        if entry is None or 'Exec' not in entry:
            print('leste-apps: no desktop entry for', app, file=sys.stderr)
            continue
        wanted.add(app)
        icon = find_icon(entry.get('Icon'))
        icon_path = ''
        if icon:
            icon_path = '/usr/share/pixmaps/pro1x-leste/%s.png' % app
            shutil.copyfile(icon, os.path.join(harmattan, icon_path.lstrip('/')))
        name = entry.get('Name', app)
        with open(os.path.join(apps_dir, PREFIX + app + '.desktop'), 'w') as f:
            f.write('[Desktop Entry]\nType=Application\n')
            f.write('Name=%s\n' % name)
            if icon_path:
                f.write('Icon=%s\n' % icon_path)
            f.write('Exec=/usr/bin/dbus-send --system --type=method_call --dest=%s %s %s.Launch string:%s\n'
                    % (NAME, PATH, NAME, app))
            f.write('X-Pro1X-Leste-App=%s\n' % app)
    for path in glob.glob(os.path.join(apps_dir, PREFIX + '*.desktop')):
        app = os.path.basename(path)[len(PREFIX):-len('.desktop')]
        if app not in wanted:
            os.remove(path)


def session_env():
    env = {'HOME': '/home/' + USER, 'USER': USER, 'LOGNAME': USER, 'DISPLAY': ':0',
           'XAUTHORITY': '/home/%s/.Xauthority' % USER, 'LANG': os.environ.get('LANG', 'en_US.UTF-8'),
           'PATH': '/usr/local/bin:/usr/bin:/bin:/usr/games',
           # Leste's Qt 5 programs crash on touch input after a blank with XI2
           'QT_XCB_NO_XI2': '1',
           # Firefox scrolls by touch only with XI2 touch events
           'MOZ_USE_XINPUT2': '1'}
    try:
        with open('/tmp/session_bus_address.user') as f:
            for line in f:
                if 'DBUS_SESSION_BUS_ADDRESS=' in line:
                    env['DBUS_SESSION_BUS_ADDRESS'] = line.split('=', 1)[1].strip().strip('\'"')
    except OSError:
        pass
    uid = int(subprocess.run(['id', '-u', USER], capture_output=True, text=True).stdout or 1000)
    env['XDG_RUNTIME_DIR'] = '/run/user/%d' % uid
    return env


def launch(app):
    if app not in listed():
        return False
    entry = desktop_entry(app)
    if entry is None:
        return False
    args = [a for a in shlex.split(entry['Exec']) if not (a.startswith('%') and len(a) == 2)]
    runuser = shutil.which('runuser', path='/usr/sbin:/sbin:/usr/bin:/bin') or '/usr/sbin/runuser'
    subprocess.Popen([runuser, '-u', USER, '--'] + args, env=session_env(), cwd='/home/' + USER,
                     stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                     start_new_session=True)
    return True


def serve():
    import dbus
    import dbus.service
    from dbus.mainloop.glib import DBusGMainLoop
    from gi.repository import GLib

    class Launcher(dbus.service.Object):
        @dbus.service.method(NAME, in_signature='s', out_signature='b')
        def Launch(self, app):
            return launch(str(app))

    DBusGMainLoop(set_as_default=True)
    bus = dbus.SystemBus()
    name = dbus.service.BusName(NAME, bus, do_not_queue=True)
    Launcher(bus, PATH)
    loop = GLib.MainLoop()
    GLib.unix_signal_add(GLib.PRIORITY_DEFAULT, 15, loop.quit)
    loop.run()


if __name__ == '__main__':
    if len(sys.argv) == 3 and sys.argv[1] == 'sync':
        sync(sys.argv[2])
    elif len(sys.argv) == 2 and sys.argv[1] == 'serve':
        serve()
    else:
        sys.exit('usage: leste-apps.py sync <harmattan tree> | serve')
