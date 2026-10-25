/*
 * pro1x-theme-scale: a copy of a Hildon theme for a screen with more pixels
 * per inch than the N900's.
 *
 *     pro1x-theme-scale [-r ROOT] SRC DEST SCALE
 *
 * Writes the theme directory SRC (e.g. /usr/share/themes/alpha) to DEST,
 * which must not exist yet:
 *   - every PNG image scaled by SCALE, except backgrounds/ (wallpapers are
 *     stretched to the screen anyway),
 *   - gtk-2.0/gtkrc with its lengths scaled: image borders, widget
 *     thicknesses, the style properties that are pixels, the icon sizes
 *     (libhildon's own sizes added, so HILDON_ICON_PIXEL_SIZE_* follow);
 *     fonts are in points and follow the X resolution instead (96 * SCALE
 *     dpi, set by the session),
 *   - matchbox2/theme.xml (and matchbox/) with the window decorations'
 *     geometry and pixel font sizes scaled,
 *   - everything else as symbolic links to SRC,
 *   - last .pro1x-theme with SRC and SCALE, which marks DEST complete.
 *
 * Every length becomes lround(length * SCALE), image sizes included, so
 * sprite rectangles and images agree. Image borders are mapped as
 * positions (the right border starts at lround((width - right) * SCALE)),
 * which keeps left + right within the scaled width.
 *
 * ROOT is prepended to SRC and DEST for reading and writing only, to build
 * a theme into a root filesystem tree: links and paths in the theme are
 * written without it.
 */

#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <glib.h>
#include <gdk-pixbuf/gdk-pixbuf.h>

static double scale;
static const char *root = "";
static const char *dest_top;
static unsigned n_images, n_links, n_lengths;

static int
px (double v)
{
  return (int) lround (v * scale);
}

static int
px_size (int v)
{
  return MAX (1, px (v));
}

static char *
rooted (const char *path)
{
  return g_strconcat (root, path, NULL);
}

static void G_GNUC_PRINTF (1, 2) G_GNUC_NORETURN
die (const char *fmt, ...)
{
  va_list ap;

  va_start (ap, fmt);
  fprintf (stderr, "pro1x-theme-scale: ");
  vfprintf (stderr, fmt, ap);
  fputc ('\n', stderr);
  va_end (ap);
  exit (1);
}

static void
link_to (const char *src, const char *dst)
{
  char *t = rooted (dst);

  if (symlink (src, t) != 0)
    die ("symlink %s: %s", t, strerror (errno));
  n_links++;
  g_free (t);
}

static gboolean
scale_png (const char *src, const char *dst)
{
  GError *err = NULL;
  GdkPixbuf *in, *out;
  char *s = rooted (src), *d = rooted (dst);
  gboolean ok = FALSE;

  in = gdk_pixbuf_new_from_file (s, &err);
  if (in)
    {
      out = gdk_pixbuf_scale_simple (in,
                                     px_size (gdk_pixbuf_get_width (in)),
                                     px_size (gdk_pixbuf_get_height (in)),
                                     GDK_INTERP_BILINEAR);
      ok = gdk_pixbuf_save (out, d, "png", &err, "compression", "3", NULL);
      g_object_unref (out);
      g_object_unref (in);
    }
  if (!ok)
    {
      fprintf (stderr, "pro1x-theme-scale: %s: %s (linked as it is)\n", s,
               err->message);
      g_error_free (err);
    }
  else
    n_images++;
  g_free (s);
  g_free (d);
  return ok;
}

/* gtkrc */

/* Style properties and settings that are not lengths */
static gboolean
is_length (const char *name)
{
  static const char *const not_lengths[] = {
    "interior-focus", "hildon-mode", "focus-handling", "position-theming",
    "wide-separators", "side-details", "state-hint", "autodimmed",
    "draw-trough", "double-arrows", "activate-slider", "box-layout",
    "include-arrow", "row-ending-details", "custom-background",
    "dislocation", "appears-as-list", "has-frame", "two-part-trough",
    "alignment", "num-buttons", "scaling", "color", NULL
  };
  const char *prop = strstr (name, "::");
  char *p;
  gboolean yes = TRUE;
  int i;

  if (!prop)
    /* Settings: two of them are distances */
    return strcmp (name, "gtk-double-click-distance") == 0 ||
           strcmp (name, "gtk-dnd-drag-threshold") == 0 ||
           (strncmp (name, "gtk-", 4) != 0 && strncmp (name, "hildon-", 7) != 0);

  p = g_ascii_strdown (prop + 2, -1);
  g_strdelimit (p, "_", '-');
  for (i = 0; not_lengths[i]; i++)
    if (strstr (p, not_lengths[i]))
      yes = FALSE;
  g_free (p);
  return yes;
}

/* "name=w,h:..." with every size scaled and libhildon's sizes added */
static char *
scale_icon_sizes (const char *v)
{
  static const struct { const char *name; int size; } hildon[] = {
    { "hildon-xsmall", 16 }, { "hildon-small", 24 }, { "hildon-stylus", 32 },
    { "hildon-finger", 48 }, { "hildon-thumb", 64 }, { "hildon-large", 96 },
    { "hildon-xlarge", 128 },
  };
  GString *out = g_string_new (NULL);
  char **items = g_strsplit (v, ":", -1);
  int i, w, h;
  char name[64];

  for (i = 0; items[i]; i++)
    if (sscanf (items[i], " %63[^= ] = %d , %d", name, &w, &h) == 3)
      g_string_append_printf (out, "%s=%d,%d:", name, px_size (w),
                              px_size (h));
  for (i = 0; i < (int) G_N_ELEMENTS (hildon); i++)
    if (!strstr (v, hildon[i].name))
      g_string_append_printf (out, "%s=%d,%d:", hildon[i].name,
                              px_size (hildon[i].size),
                              px_size (hildon[i].size));
  g_strfreev (items);
  return g_string_free (out, FALSE);
}

/* An image's size from a gtkrc file name: relative to the gtkrc, else
 * by its base name in SRC/images */
static gboolean
image_size (const char *src_dir, const char *rcdir, const char *file,
            int *w, int *h)
{
  char *base = g_path_get_basename (file);
  char *cand[2], *r;
  gboolean ok = FALSE;
  int i;

  cand[0] = g_path_is_absolute (file) ? g_strdup (file)
                                      : g_build_filename (rcdir, file, NULL);
  cand[1] = g_build_filename (src_dir, "images", base, NULL);
  for (i = 0; i < 2 && !ok; i++)
    {
      r = rooted (cand[i]);
      ok = gdk_pixbuf_get_file_info (r, w, h) != NULL;
      g_free (r);
    }
  g_free (cand[0]);
  g_free (cand[1]);
  g_free (base);
  return ok;
}

/* The quoted value of 'key = "..."' in a line, or NULL */
static char *
quoted_value (const char *line, const char *key)
{
  const char *p = line;
  size_t n = strlen (key);
  const char *q;

  while (*p == ' ' || *p == '\t')
    p++;
  if (strncmp (p, key, n) != 0)
    return NULL;
  p += n;
  while (*p == ' ' || *p == '\t')
    p++;
  if (*p++ != '=')
    return NULL;
  while (*p == ' ' || *p == '\t')
    p++;
  if (*p++ != '"' || !(q = strchr (p, '"')))
    return NULL;
  return g_strndup (p, q - p);
}

/* One line of the gtkrc. size_* is the image of the surrounding image
 * block for border (w >= 0), overlay_border (ow >= 0). */
static char *
rewrite_rc_line (const char *line, int w, int h, int ow, int oh)
{
  const char *p = line, *name, *v;
  char *key, *rest, *out;
  size_t indent, nlen;
  long n[8];
  int k, iw, ih;

  while (*p == ' ' || *p == '\t')
    p++;
  indent = p - line;

  if (strncmp (p, "pixmap_path", 11) == 0)
    {
      /* Our images instead of the original's */
      char *path = g_build_filename (dest_top, "images", NULL);
      out = g_strdup_printf ("%.*spixmap_path \"%s\"\n", (int) indent, line,
                             path);
      g_free (path);
      return out;
    }

  name = p;
  while (g_ascii_isalnum (*p) || *p == '_' || *p == '-' || *p == ':')
    p++;
  nlen = p - name;
  if (nlen == 0)
    return NULL;
  if (*p == '[')                        /* fg[NORMAL] and the like */
    return NULL;
  while (*p == ' ' || *p == '\t')
    p++;
  if (*p != '=')
    return NULL;
  p++;
  while (*p == ' ' || *p == '\t')
    p++;
  v = p;
  key = g_strndup (name, nlen);

  if (strcmp (key, "gtk-icon-sizes") == 0)
    {
      char *sizes = quoted_value (line, "gtk-icon-sizes"), *s;
      out = NULL;
      if (sizes)
        {
          s = scale_icon_sizes (sizes);
          out = g_strdup_printf ("%.*sgtk-icon-sizes = \"%s\"\n",
                                 (int) indent, line, s);
          g_free (s);
          g_free (sizes);
        }
      g_free (key);
      return out;
    }

  if (*v == '{')
    {
      /* { a, b, c, d } */
      const char *q = v + 1, *end = strchr (v, '}');
      char *e;
      int cnt = 0;

      if (!end || memchr (v, '.', end - v) || (!is_length (key) &&
          strcmp (key, "border") != 0 && strcmp (key, "overlay_border") != 0))
        {
          g_free (key);
          return NULL;
        }
      while (q < end && cnt < 8)
        {
          while (q < end && (*q == ' ' || *q == '\t' || *q == ','))
            q++;
          if (q >= end)
            break;
          n[cnt] = strtol (q, &e, 10);
          if (e == q)
            {
              g_free (key);
              return NULL;
            }
          cnt++;
          q = e;
        }
      iw = strcmp (key, "border") == 0 ? w :
           strcmp (key, "overlay_border") == 0 ? ow : -1;
      ih = strcmp (key, "border") == 0 ? h :
           strcmp (key, "overlay_border") == 0 ? oh : -1;
      if (cnt == 4 && iw > 0 && ih > 0)
        {
          /* left, right, top, bottom as positions in the image */
          long l = px (n[0]), r = px_size (iw) - px (iw - n[1]);
          long t = px (n[2]), b = px_size (ih) - px (ih - n[3]);
          n[0] = l; n[1] = r; n[2] = t; n[3] = b;
        }
      else
        for (k = 0; k < cnt; k++)
          n[k] = px (n[k]);
      rest = g_strdup (end + 1);
      {
        GString *s = g_string_new (NULL);
        g_string_append_printf (s, "%.*s%s = {", (int) indent, line, key);
        for (k = 0; k < cnt; k++)
          g_string_append_printf (s, "%s %ld", k ? "," : "", n[k]);
        g_string_append_printf (s, " }%s", rest);
        out = g_string_free (s, FALSE);
      }
      g_free (rest);
      g_free (key);
      n_lengths += cnt;
      return out;
    }

  if ((g_ascii_isdigit (*v) || (*v == '-' && g_ascii_isdigit (v[1]))) &&
      is_length (key))
    {
      char *e;
      long x = strtol (v, &e, 10);

      if (*e == '.' || g_ascii_isalpha (*e) || *e == '_')
        {
          g_free (key);
          return NULL;
        }
      out = g_strdup_printf ("%.*s%d%s", (int) (v - line), line, px (x), e);
      g_free (key);
      n_lengths++;
      return out;
    }

  g_free (key);
  return NULL;
}

static void
rewrite_gtkrc (const char *src_dir, const char *src, const char *dst)
{
  char *s = rooted (src), *d = rooted (dst), *text, *rcdir;
  char **lines;
  GString *out = g_string_new (NULL);
  GError *err = NULL;
  int i, j, start, w, h, ow, oh;

  if (!g_file_get_contents (s, &text, NULL, &err))
    die ("%s", err->message);
  rcdir = g_path_get_dirname (src);
  lines = g_strsplit (text, "\n", -1);

  for (i = 0; lines[i]; i++)
    {
      const char *p = lines[i];
      char *r;

      while (*p == ' ' || *p == '\t')
        p++;
      w = h = ow = oh = -1;
      start = i;
      /* "image" (then "{" on this line or the next) up to its "}" */
      if (strncmp (p, "image", 5) == 0 &&
          (p[5] == 0 || p[5] == ' ' || p[5] == '\t' || p[5] == '{' ||
           p[5] == '\r'))
        {
          for (j = i + 1; lines[j]; j++)
            {
              const char *t = lines[j];
              char *f;

              while (*t == ' ' || *t == '\t')
                t++;
              if (*t == '}')
                break;
              if ((f = quoted_value (lines[j], "file")))
                {
                  if (!image_size (src_dir, rcdir, f, &w, &h))
                    w = h = -1;
                  g_free (f);
                }
              if ((f = quoted_value (lines[j], "overlay_file")))
                {
                  if (!image_size (src_dir, rcdir, f, &ow, &oh))
                    ow = oh = -1;
                  g_free (f);
                }
            }
          for (; lines[i] && (i == start || i <= j); i++)
            {
              r = rewrite_rc_line (lines[i], w, h, ow, oh);
              g_string_append (out, r ? r : lines[i]);
              if (!r || !g_str_has_suffix (r, "\n"))
                g_string_append_c (out, '\n');
              g_free (r);
            }
          i--;
          continue;
        }
      r = rewrite_rc_line (lines[i], -1, -1, -1, -1);
      g_string_append (out, r ? r : lines[i]);
      if (lines[i + 1] && (!r || !g_str_has_suffix (r, "\n")))
        g_string_append_c (out, '\n');
      g_free (r);
    }

  if (!g_file_set_contents (d, out->str, out->len, &err))
    die ("%s", err->message);
  g_string_free (out, TRUE);
  g_strfreev (lines);
  g_free (rcdir);
  g_free (text);
  g_free (s);
  g_free (d);
}

/* matchbox theme.xml */

static gboolean
xml_attr (const GMatchInfo *m, GString *out, gpointer data G_GNUC_UNUSED)
{
  char *name = g_match_info_fetch (m, 1);
  char *num = g_match_info_fetch (m, 2);
  char *unit = g_match_info_fetch (m, 3);

  if (strcmp (unit, "pt") == 0)
    {
      char *all = g_match_info_fetch (m, 0);
      g_string_append (out, all);
      g_free (all);
    }
  else
    {
      g_string_append_printf (out, "%s=\"%d%s\"", name, px (atoi (num)),
                              unit);
      n_lengths++;
    }
  g_free (name);
  g_free (num);
  g_free (unit);
  return FALSE;
}

static void
rewrite_xml (const char *src, const char *dst)
{
  char *s = rooted (src), *d = rooted (dst), *text, *out;
  GError *err = NULL;
  GRegex *re;

  if (!g_file_get_contents (s, &text, NULL, &err))
    die ("%s", err->message);
  re = g_regex_new ("(?<![A-Za-z-])(x|y|width|height|font-size|"
                    "template-[a-z-]+)=\"(-?[0-9]+)([a-z]*)\"", 0, 0, NULL);
  out = g_regex_replace_eval (re, text, -1, 0, 0, xml_attr, NULL, &err);
  if (!out || !g_file_set_contents (d, out, -1, &err))
    die ("%s", err->message);
  g_regex_unref (re);
  g_free (out);
  g_free (text);
  g_free (s);
  g_free (d);
}

static void
copy_dir (const char *src_top, const char *src, const char *dst, int level)
{
  char *rs = rooted (src), *rd = rooted (dst);
  DIR *dir;
  struct dirent *e;
  struct stat st;

  if (mkdir (rd, 0755) != 0)
    die ("mkdir %s: %s", rd, strerror (errno));
  if (!(dir = opendir (rs)))
    die ("%s: %s", rs, strerror (errno));
  while ((e = readdir (dir)))
    {
      char *s, *d, *r, *parent;

      if (strcmp (e->d_name, ".") == 0 || strcmp (e->d_name, "..") == 0)
        continue;
      s = g_build_filename (src, e->d_name, NULL);
      d = g_build_filename (dst, e->d_name, NULL);
      r = rooted (s);
      parent = g_path_get_basename (src);
      if (lstat (r, &st) != 0)
        die ("%s: %s", r, strerror (errno));
      if (S_ISDIR (st.st_mode))
        {
          if (level == 0 && strcmp (e->d_name, "backgrounds") == 0)
            link_to (s, d);
          else
            copy_dir (src_top, s, d, level + 1);
        }
      else if (S_ISREG (st.st_mode) && g_str_has_suffix (e->d_name, ".png"))
        {
          if (!scale_png (s, d))
            link_to (s, d);
        }
      else if (S_ISREG (st.st_mode) && level == 1 &&
               strcmp (parent, "gtk-2.0") == 0 &&
               strcmp (e->d_name, "gtkrc") == 0)
        rewrite_gtkrc (src_top, s, d);
      else if (S_ISREG (st.st_mode) && level == 1 &&
               g_str_has_prefix (parent, "matchbox") &&
               strcmp (e->d_name, "theme.xml") == 0)
        rewrite_xml (s, d);
      else
        link_to (s, d);
      g_free (parent);
      g_free (r);
      g_free (s);
      g_free (d);
    }
  closedir (dir);
  g_free (rs);
  g_free (rd);
}

int
main (int argc, char **argv)
{
  char *src, *dst, *end, *r;
  int opt;

  while ((opt = getopt (argc, argv, "r:")) != -1)
    {
      if (opt != 'r')
        goto usage;
      root = optarg;
    }
  if (argc - optind != 3)
    goto usage;
  src = g_strdup (argv[optind]);
  dst = g_strdup (argv[optind + 1]);
  scale = g_ascii_strtod (argv[optind + 2], &end);
  if (*end || !(scale >= 0.5 && scale <= 8))
    die ("bad scale %s (0.5 to 8)", argv[optind + 2]);
  if (!g_path_is_absolute (src) || !g_path_is_absolute (dst))
    die ("SRC and DEST must be absolute paths");
  while (strlen (src) > 1 && g_str_has_suffix (src, "/"))
    src[strlen (src) - 1] = 0;
  while (strlen (dst) > 1 && g_str_has_suffix (dst, "/"))
    dst[strlen (dst) - 1] = 0;
  dest_top = dst;
  r = rooted (dst);
  if (access (r, F_OK) == 0)
    die ("%s exists", r);
  g_free (r);

  copy_dir (src, src, dst, 0);
  /* Last: the theme is complete */
  {
    char *mark = g_build_filename (dst, ".pro1x-theme", NULL);
    char *rm = rooted (mark);
    char *text = g_strdup_printf ("%s\n%s\n", src, argv[optind + 2]);
    GError *err = NULL;

    if (!g_file_set_contents (rm, text, -1, &err))
      die ("%s", err->message);
    g_free (text);
    g_free (rm);
    g_free (mark);
  }
  printf ("pro1x-theme-scale: %s -> %s x%g: %u images, %u lengths, "
          "%u links\n", src, dst, scale, n_images, n_lengths, n_links);
  return 0;

usage:
  fprintf (stderr, "usage: pro1x-theme-scale [-r ROOT] SRC DEST SCALE\n");
  return 2;
}
