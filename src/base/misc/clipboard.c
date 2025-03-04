#include <string.h>
#include "dosemu_debug.h"
#include "types.h"
#include "translate/translate.h"
#include "clipboard.h"

struct clipboard_system *Clipboard;

static char *clipboard_make_str_utf8(int type, const char *p, int size)
{
  char *q;

  if (type == CF_TEXT) {
    q = strndup(p, size);
  } else { // CF_OEMTEXT
    struct char_set_state state;
    int characters;
    t_unicode *str;

    init_charset_state(&state, trconfig.dos_charset);

    characters = character_count(&state, p, size);
    if (characters == -1) {
      v_printf("SDL_clipboard: Write invalid char count\n");
      return NULL;
    }

    str = malloc(sizeof(t_unicode) * (characters + 1));
    charset_to_unicode_string(&state, str, &p, size, characters + 1);
    cleanup_charset_state(&state);
    q = unicode_string_to_charset((wchar_t *)str, "utf8");
    free(str);
  }
  return q;
}

static char *clipboard_make_str_dos(int type, const char *p, int size)
{
  char *q;

  if (type == CF_TEXT) {
    q = strndup(p, size);
  } else { // CF_OEMTEXT
    struct char_set_state state;
    struct char_set *utf8;
    int characters;
    t_unicode *str;

    utf8 = lookup_charset("utf8");
    init_charset_state(&state, utf8);

    characters = character_count(&state, p, size);
    if (characters == -1) {
      v_printf("SDL_clipboard: _clipboard_grab_data()  invalid char count\n");
      return NULL;
    }

    str = malloc(sizeof(t_unicode) * (characters + 1));
    charset_to_unicode_string(&state, str, &p, size, characters + 1);
    cleanup_charset_state(&state);
    q = unicode_string_to_charset((wchar_t *)str, trconfig.dos_charset->names[0]);
    free(str);
  }
  return q;
}

int register_clipboard_system(struct clipboard_system *cs)
{
  Clipboard = cs;
  return 1;
}

char *clip_str;

static void do_clear(void)
{
  free(clip_str);
  clip_str = NULL;
}

void add_clip_str(char *q)
{
  if (clip_str) {
    clip_str = realloc(clip_str, strlen(clip_str) + strlen(q) + 1);
    strcat(clip_str, q);
    free(q);
  } else {
    clip_str = q;
  }
}

int cnn_clear(void)
{
  do_clear();
  return TRUE;
}

int cnn_write(int type, const char *p, int size)
{
  char *q;

  if (type != CF_TEXT && type != CF_OEMTEXT) {
    v_printf("SDL_clipboard: Write failed, type (0x%02x) unsupported\n", type);
    return FALSE;
  }
  q = clipboard_make_str_utf8(type, p, size);
  if (!q)
    return FALSE;
  add_clip_str(q);
  return TRUE;
}

int cnn_getsize(int type)
{
  char *q;
  int ret;

  if (type == CF_TEXT)
    v_printf("SDL_clipboard: GetSize of type CF_TEXT\n");
  else if (type == CF_OEMTEXT)
    v_printf("SDL_clipboard: GetSize of type CF_OEMTEXT\n");
  else {
    v_printf("SDL_clipboard: GetSize failed (type 0x%02x unsupported\n", type);
    return 0;
  }

  if (!clip_str) {
    v_printf("SDL_clipboard: GetSize failed (grabbed data is NULL\n");
    return 0;
  }
  q = clipboard_make_str_dos(type, clip_str, strlen(clip_str));
  if (!q)
    return 0;
  ret = strlen(clip_str) + 1;
  free(q);
  return ret;
}

int cnn_getdata(int type, char *p, int size)
{
  char *q;

  if (!clip_str)
    return FALSE;
  q = clipboard_make_str_dos(type, clip_str, strlen(clip_str));
  if (!q)
    return FALSE;
  strlcpy(p, q, size);
  free(q);
  return TRUE;
}
