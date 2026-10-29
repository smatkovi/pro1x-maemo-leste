#!/bin/sh
# events-fixture.sh DIR: made-up content for the Events page
# (tools/pro1x-events) in the test VM, nobody's real data:
# - DIR/api/v1/timelines/home, a Mastodon home timeline of eight posts by
#   made-up accounts (one boosted, one with a content warning, one with
#   pictures), their avatars as DIR/avatars/*.png, for
#   python3 -m http.server in DIR (try-meego-events.sh EVENTS_FIXTURE=DIR)
# - unread events in ~/.rtcom-eventlogger/el-v1.db: two missed calls, two
#   SMS of one sender, a chat message (with the sqlite3 tool; the page reads
#   them through rtcom-eventlogger)
set -eu
DIR=${1:?usage: events-fixture.sh DIR}
URL=${URL:-http://127.0.0.1:8000}
mkdir -p "$DIR/api/v1/timelines" "$DIR/avatars"
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
    post(3, 2, 42, '<p>Spoilers inside</p>', spoiler_text='Film ending', sensitive=True),
    post(4, 3, 75, '<p>Two pictures from the walk this afternoon.</p>',
         media_attachments=[{'type': 'image'}, {'type': 'image'}]),
    post(5, 0, 130, '', reblog=post(6, 2, 140, '<p>A boosted post: shared by Ada, written by Cleo.</p>')),
    post(7, 1, 60 * 20, '<p>Yesterday\'s post, with an emoji ☺ and a line<br>break.</p>'),
    post(8, 3, 60 * 50, '<p>Older post from two days ago. ' + 'Long text that wraps over several lines. ' * 8 + '</p>'),
]
json.dump(feed, open('%s/api/v1/timelines/home' % d, 'w'))
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
echo "fixture in $DIR"
