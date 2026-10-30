#!/bin/sh
# events-fixture.sh DIR: made-up content for the Events page
# (tools/pro1x-events) in the test VM, nobody's real data:
# - DIR/api/v1/timelines/home, a Mastodon home timeline of eight posts by
#   made-up accounts (one boosted, one with a content warning and its
#   pictures marked sensitive, one with two pictures, one with three, one
#   with a video), their avatars as DIR/avatars/*.png, the pictures' and
#   the video's previews (made-up stripes and discs) as DIR/media/*.png, for
#   python3 -m http.server in DIR (try-meego-events.sh EVENTS_FIXTURE=DIR)
# - unread events in ~/.rtcom-eventlogger/el-v1.db: two missed calls, two
#   SMS of one sender, a chat message (with the sqlite3 tool; the page reads
#   them through rtcom-eventlogger)
# - DIR/v1/forecast, an answer in Open-Meteo's format (made-up weather for
#   today and four more days), DIR/v1/search one of its geocoding (three
#   places called Wien), for the page's and its applet's weather_url and
#   geocoding_url
# - appointments in calendar-backend's database (the one qalendar uses):
#   one over (not shown), one now, one later today, an all-day one, a
#   weekly one (its instance tomorrow), a task, a birthday (yearly), in
#   three calendars of their own colours; the database made first by
#   calendar-backend itself (its calendarclt tool) where there is none
set -eu
DIR=${1:?usage: events-fixture.sh DIR}
URL=${URL:-http://127.0.0.1:8000}
mkdir -p "$DIR/api/v1/timelines" "$DIR/avatars" "$DIR/media" "$DIR/v1"
python3.11 - "$DIR" "$URL" <<'PY'
import json, struct, sys, time, zlib
d, url = sys.argv[1:3]
def png(path, rgb):
    # a plain square of one colour with a lighter disc, 96 px
    w = h = 96
    rows = b''
    for y in range(h):
        row = b'\0'
        for x in range(w):
            r2 = (x - 48) ** 2 + (y - 40) ** 2
            c = tuple(min(255, v + 90) for v in rgb) if r2 < 22 ** 2 or \
                ((x - 48) ** 2 / 1600 + (y - 100) ** 2 / 1100 < 1) else rgb
            row += bytes(c)
        rows += row
    def chunk(t, b):
        return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b) & 0xffffffff)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
                           + chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))
def picture(path, w, h, rgb):
    # a made-up picture: stripes across, a light disc in the middle (to see
    # how it is cropped), w x h
    rows = b''
    for y in range(h):
        row = b'\0'
        for x in range(w):
            k = 1.0 if (x * 8 // w) % 2 else 0.6
            c = tuple(int(v * k) for v in rgb)
            if (x - w / 2) ** 2 + (y - h / 2) ** 2 < (min(w, h) / 4) ** 2:
                c = (235, 235, 220)
            row += bytes(c)
        rows += row
    def chunk(t, b):
        return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b) & 0xffffffff)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
                           + chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))
pictures = [('m0', 320, 200, (200, 120, 40)), ('m1', 200, 320, (60, 140, 200)),
            ('m2', 240, 240, (90, 170, 90)), ('m3', 320, 180, (170, 70, 150)),
            ('m4', 300, 200, (120, 120, 200)), ('v0', 320, 180, (60, 60, 60))]
for n, w, h, rgb in pictures:
    picture('%s/media/%s.png' % (d, n), w, h, rgb)
def media(kind, name):
    return {'id': name, 'type': kind, 'url': '%s/media/%s.png' % (url, name),
            'preview_url': '%s/media/%s.png' % (url, name), 'description': None}
people = [('Ada Example', 'ada@example.social', (40, 90, 160)),
          ('Bo Testperson', 'bo@demo.example', (150, 60, 60)),
          ('Cleo Sample', 'cleo', (50, 130, 70)),
          ('Dev Placeholder', 'dev@test.example', (120, 80, 150))]
for i, p in enumerate(people):
    png('%s/avatars/a%d.png' % (d, i), p[2])
def acct(i):
    p = people[i]
    return {'id': str(100 + i), 'username': p[1].split('@')[0], 'acct': p[1],
            'display_name': p[0], 'avatar': '%s/avatars/a%d.png' % (url, i),
            'avatar_static': '%s/avatars/a%d.png' % (url, i)}
now = time.time()
def post(n, who, minutes, html, **kw):
    s = {'id': str(1000 + n), 'created_at': time.strftime('%Y-%m-%dT%H:%M:%S.000Z',
                                                         time.gmtime(now - minutes * 60)),
         'url': 'https://example.social/@%s/%d' % (people[who][1].split('@')[0], 1000 + n),
         'uri': 'https://example.social/users/x/statuses/%d' % (1000 + n),
         'content': html, 'spoiler_text': '', 'sensitive': False, 'reblog': None,
         'account': acct(who), 'media_attachments': []}
    s.update(kw)
    return s
feed = [
    post(1, 0, 3, '<p>Good morning! The tram was on time today, which is worth a post.</p>'),
    post(2, 1, 17, '<p>Testing the new <a href="https://example.org/"><span class="invisible">https://</span>'
                   '<span class="ellipsis">example.org/a-very-long-p</span><span class="invisible">ath</span></a> '
                   'page &amp; it works.</p><p>Second paragraph with <strong>bold</strong> text.</p>'),
    post(3, 2, 42, '<p>Spoilers inside</p>', spoiler_text='Film ending', sensitive=True,
         media_attachments=[media('image', 'm4')]),
    post(4, 3, 75, '<p>Two pictures from the walk this afternoon.</p>',
         media_attachments=[media('image', 'm0'), media('image', 'm1')]),
    post(9, 1, 90, '<p>Three of the market.</p>',
         media_attachments=[media('image', 'm2'), media('image', 'm3'), media('image', 'm0'),
                            media('image', 'm1')]),
    post(10, 2, 110, '<p>The fountain, a short video.</p>',
         media_attachments=[media('video', 'v0')]),
    post(5, 0, 130, '', reblog=post(6, 2, 140, '<p>A boosted post: shared by Ada, written by Cleo.</p>')),
    post(7, 1, 60 * 20, '<p>Yesterday\'s post, with an emoji ☺ and a line<br>break.</p>'),
    post(8, 3, 60 * 50, '<p>Older post from two days ago. ' + 'Long text that wraps over several lines. ' * 8 + '</p>'),
]
json.dump(feed, open('%s/api/v1/timelines/home' % d, 'w'))

# the weather, in Open-Meteo's format (current=..., daily=..., timezone=auto)
day = 86400
days = [time.strftime('%Y-%m-%d', time.localtime(now + i * day)) for i in range(5)]
weather = {
    'latitude': 48.2, 'longitude': 16.38, 'timezone': 'Europe/Vienna',
    'utc_offset_seconds': time.localtime(now).tm_gmtoff,
    'current_units': {'temperature_2m': '°C', 'wind_speed_10m': 'km/h'},
    'current': {'time': time.strftime('%Y-%m-%dT%H:%M', time.localtime(now)),
                'temperature_2m': 9.4, 'apparent_temperature': 6.8, 'weather_code': 2,
                'is_day': 1, 'wind_speed_10m': 14.2},
    'daily': {'time': days, 'weather_code': [2, 63, 95, 0, 73],
              'temperature_2m_max': [11.6, 9.8, 13.1, 12.4, 2.3],
              'temperature_2m_min': [4.2, 6.0, 7.5, 1.9, -3.4],
              'precipitation_probability_max': [10, 80, 65, 0, 70]},
}
json.dump(weather, open('%s/v1/forecast' % d, 'w'))
places = {'results': [
    {'name': 'Wien', 'latitude': 48.20849, 'longitude': 16.37208, 'country': 'Österreich',
     'admin1': 'Wien', 'timezone': 'Europe/Vienna'},
    {'name': 'Wien', 'latitude': 39.66086, 'longitude': -92.78436,
     'country': 'Vereinigte Staaten', 'admin1': 'Missouri', 'timezone': 'America/Chicago'},
    {'name': 'Wien', 'latitude': 44.90191, 'longitude': -90.02458,
     'country': 'Vereinigte Staaten', 'admin1': 'Wisconsin', 'timezone': 'America/Chicago'}]}
json.dump(places, open('%s/v1/search' % d, 'w'))
PY
DB=$HOME/.rtcom-eventlogger/el-v1.db
if command -v sqlite3 > /dev/null && [ -f "$DB" ]; then
	now=$(date +%s)
	sqlite3 "$DB" <<SQL
DELETE FROM Events WHERE local_uid = 'fixture';
INSERT OR REPLACE INTO Remotes (local_uid, remote_uid, remote_name) VALUES
  ('fixture', '+431234567', 'Erika Example'),
  ('fixture', '+439876543', 'Fritz Test'),
  ('fixture', 'gina@chat.example', 'Gina Sample');
INSERT INTO Events (service_id, event_type_id, storage_time, start_time, end_time,
  is_read, outgoing, local_uid, remote_uid, free_text)
SELECT s.id, t.id, $now - x.ago, $now - x.ago, $now - x.ago, 0, 0, 'fixture', x.remote, x.txt
FROM (SELECT 'RTCOM_EL_SERVICE_CALL' AS s, 'RTCOM_EL_EVENTTYPE_CALL_MISSED' AS t,
             '+431234567' AS remote, NULL AS txt, 600 AS ago
      UNION ALL SELECT 'RTCOM_EL_SERVICE_CALL', 'RTCOM_EL_EVENTTYPE_CALL_MISSED',
             '+431234567', NULL, 300
      UNION ALL SELECT 'RTCOM_EL_SERVICE_SMS', 'RTCOM_EL_EVENTTYPE_SMS_MESSAGE',
             '+439876543', 'Are we still meeting at six?', 1500
      UNION ALL SELECT 'RTCOM_EL_SERVICE_SMS', 'RTCOM_EL_EVENTTYPE_SMS_MESSAGE',
             '+439876543', 'I am at the station now.', 240
      UNION ALL SELECT 'RTCOM_EL_SERVICE_CHAT', 'RTCOM_EL_EVENTTYPE_CHAT_MESSAGE',
             'gina@chat.example', 'The files are uploaded.', 3600) x
JOIN Services s ON s.name = x.s JOIN EventTypes t ON t.name = x.t;
SQL
	echo "rtcom-eventlogger: $(sqlite3 "$DB" "SELECT count(*) FROM Events WHERE local_uid = 'fixture' AND is_read = 0") unread events"
fi
# calendar-backend's database: $HOME/.calendar/calendardb, for root
# /home/$USER's (/home/user's), as calendar-backend has it
if [ -n "${HOME:-}" ] && [ "$HOME" != /root ]; then
	CALDIR=$HOME/.calendar
elif [ -n "${USER:-}" ] && [ "$USER" != root ]; then
	CALDIR=/home/$USER/.calendar
else
	CALDIR=/home/user/.calendar
fi
CALDB=$CALDIR/calendardb
if command -v sqlite3 > /dev/null && command -v calendarclt > /dev/null; then
	# (listing the calendars makes calendar-backend create its database)
	[ -f "$CALDB" ] || printf '4\n8\n' | dbus-run-session -- calendarclt > /dev/null 2>&1 || true
fi
if command -v sqlite3 > /dev/null && [ -f "$CALDB" ]; then
	# (the SQL from Python, which has no sqlite3 module here)
	python3.11 - <<'PY' | sqlite3 "$CALDB"
import time
# (calendar-backend keeps a zone as libtime names it, ":Europe/Vienna")
try:
    tz = ':' + open('/var/db/zoneinfo').read().strip()
except OSError:
    tz = ':UTC'
out = []
q = out.append
# its two default calendars (as calendar-backend makes them) where there
# are none, a choir's and the birthdays'
q("INSERT OR IGNORE INTO Calendars VALUES (1, 'cal_ti_calendar_synced', 0, 1, 0, 4, '', '1.0', 0);")
q("INSERT OR IGNORE INTO Calendars VALUES (2, 'cal_ti_calendar_private', 1, 1, 0, 3, '', '1.0', 0);")
q("INSERT OR REPLACE INTO Calendars VALUES (90, 'Choir (fixture)', 3, 1, 0, 0, '', '1.0', 0);")
q("INSERT OR REPLACE INTO Calendars VALUES (91, 'cal_ti_smart_birthdays', 8, 1, 0, 1, '', '1.0', 0);")
for t in ('Recursive', 'ComponentDetails'):
    q("DELETE FROM %s WHERE Id IN (SELECT Id FROM Components WHERE Uid LIKE 'fixture-%%');" % t)
q("DELETE FROM Components WHERE Uid LIKE 'fixture-%';")
now = int(time.time())
lt = time.localtime(now)
def local(days, h, m=0, year=None):
    return int(time.mktime((year or lt.tm_year, lt.tm_mon, lt.tm_mday + days, h, m, 0, 0, 0, -1)))
def s(v):
    return "'%s'" % v.replace("'", "''")
def add(n, cal, kind, start, end, summary, location='', allday=0, rule=None, rtype=0, status=0):
    uid = 'fixture-%d' % n
    q('INSERT INTO Components (CalendarId, ComponentType, Flags, DateStart, DateEnd, Summary, '
      'Location, Description, Status, Uid, Until, AllDay, CreatedTime, ModifiedTime, Tzid, '
      "TzOffset) VALUES (%d, %d, %d, %d, %d, %s, %s, '', %d, %s, -1, %d, %d, %d, %s, %d);" %
      (cal, kind, 3 if rule else 0, start, end, s(summary), s(location), status, s(uid),
       allday, now, now, s(tz), time.localtime(start).tm_gmtoff))
    q('INSERT INTO ComponentDetails (Id, ComponentType) SELECT Id, %d FROM Components '
      'WHERE Uid = %s;' % (kind, s(uid)))
    if rule:
        q("INSERT INTO Recursive SELECT Id, %s, '', '', '', 0, %d FROM Components "
          'WHERE Uid = %s;' % (s(rule), rtype, s(uid)))
EVENT, TODO, BDAY = 1, 2, 4
mins = (now // 60) * 60
add(1, 2, EVENT, local(0, 0), local(0, 23, 59), 'Pay the rent', allday=1)
add(2, 2, EVENT, mins - 3 * 3600, mins - 2 * 3600, 'Breakfast meeting (over)', 'Cafe Example')
add(3, 1, EVENT, mins - 1800, mins + 3600, 'Project call', 'Room 2.14')
add(4, 2, EVENT, mins + 3 * 3600, mins + 4 * 3600, 'Dentist', 'Example Street 5')
# weekly since a week before tomorrow
add(5, 90, EVENT, local(-6, 19), local(-6, 21), 'Choir rehearsal', 'Parish hall',
    rule='FREQ=WEEKLY', rtype=3)
add(6, 2, TODO, local(2, 0), local(2, 0), 'Return the library books', allday=1)
# born in 1990, the birthday in three days
add(7, 91, BDAY, local(3, 0, year=1990), local(3, 23, 59, year=1990), 'Erika Example',
    allday=1, rule='FREQ=YEARLY', rtype=5, status=1990)
add(8, 2, EVENT, local(4, 14), local(4, 15), 'Haircut')
print('\n'.join(out))
PY
	echo "calendar-backend: $(sqlite3 "$CALDB" "SELECT count(*) FROM Components WHERE Uid LIKE 'fixture-%'") appointments in $CALDB"
fi
echo "fixture in $DIR"
