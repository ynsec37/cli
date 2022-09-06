
#include <string.h>
#include <stdlib.h>

#include "errors.h"
#include "vtparse.h"

#define CUR(term) (((term)->cursor_y) * ((term)->width) + ((term)->cursor_x))
#define POS(term, x, y) ((y) * ((term)->width) + (x))

#define CLI_COL_256 254
#define CLI_COL_RGB 255

struct color {
  /* 0 is off
   * 30-37, 40-47, 90-97, 100-107
   * CLI_COL_256 (254) is 8 bit
   * CLI_COL_RGB (255) is 24 bit */
  unsigned char col;
  unsigned char r, g, b;
};

struct pen {
  struct color fg;
  struct color bg;
  int bold;
  int italic;
  int underline;
  int strikethrough;
  int blink;
  int inverse;
  int link;         // 0 is no link, i is link no (i-1)
};

void cli_term_reset_pen(struct pen *pen) {
  memset(pen, 0, sizeof(struct pen));
}

typedef unsigned int CHARTYPE;

struct cell {
  CHARTYPE ch;
  struct pen pen;
};

#define OSC_LEN 1024
#define OSC_NUM_LINKS 512
#define OSC_LINK_DATA_LEN 8192

struct terminal {
  vtparse_t *vt;
  int width;
  int height;
  struct cell *screen;
  int cursor_x;
  int cursor_y;
  struct pen pen;
  CHARTYPE *osc;
  int oscptr;
  int *links;
  int linkptr;
  CHARTYPE *linkdata;
  int linkdataptr;
};

void cli_term_clear_line(struct terminal *term, int line) {
  memset(
    term->screen + line * term->width,
    0,
    sizeof(struct cell) * term->width
  );
  int i, from = term->width * line;
  int to = from + term->width;
  for (i = from; i < to; i++) {
    term->screen[i].ch = ' ';
  }
}

void cli_term_clear_screen(struct terminal *term) {
  int i, n = term->width * term->height;
  memset(term->screen, 0, sizeof(struct cell) * n);
  for (i = 0; i < n; i++) {
    term->screen[i].ch = ' ';
  }
}

int cli_term_init(struct terminal *term, int width, int height) {
  term->width = width;
  term->height = height;
  term->screen = (struct cell*) R_alloc(width * height, sizeof(struct cell));
  term->osc = NULL;
  term->oscptr = 0;
  term->links = NULL;
  term->linkptr = 0;
  term->linkdata = NULL;
  term->linkdataptr = 0;
  cli_term_clear_screen(term);
  return 0;
}

// We return a static buffer here!!!

const char *cli_term_color_fg_to_string(struct color *col) {
  static char buf[20];
  if (col->col == 0) return("");
  if (col->col == CLI_COL_256) {
    snprintf(buf, sizeof(buf), "fg:%d;", col->r);

  } else if (col->col == CLI_COL_RGB) {
    snprintf(buf, sizeof(buf), "fg:#%02x%02x%02x;", col->r, col->g, col->b);

  } else if (col->col >= 30 && col->col <= 37) {
    snprintf(buf, sizeof(buf), "fg:%d;", col->col - 30);

  } else if (col->col >= 90 && col->col <= 97) {
    snprintf(buf, sizeof(buf), "fg:%d;", col->col - 90 + 8);
  }

  return buf;
}

// We return a static buffer here!!!

const char *cli_term_color_bg_to_string(struct color *col) {
  static char buf[20];
  if (col->col == 0) return("");
  if (col->col == CLI_COL_256) {
    snprintf(buf, sizeof(buf), "bg:%d;", col->r);

  } else if (col->col == CLI_COL_RGB) {
    snprintf(buf, sizeof(buf), "bg:#%02x%02x%02x;", col->r, col->g, col->b);

  } else if (col->col >= 40 && col->col <= 47) {
    snprintf(buf, sizeof(buf), "bg:%d;", col->col - 40);

  } else if (col->col >= 100 && col->col <= 107) {
    snprintf(buf, sizeof(buf), "bg:%d;", col->col - 100 + 8);
  }

  return buf;
}

const char *cli_term_link_to_string(struct terminal *term, int linkno) {
  static char buf[20];
  snprintf(buf, sizeof(buf), "link:%d;", linkno);
  return buf;
}

SEXP cli_term_pen_to_string(struct terminal *term, struct pen *pen) {
  // TODO: calculate max possible buf length
  char buf[100];
  int ret = snprintf(
    buf,
    sizeof(buf),
    "%s%s%s%s%s%s%s%s%s",
    pen->fg.col ? cli_term_color_fg_to_string(&pen->fg) : "",
    pen->bg.col ? cli_term_color_bg_to_string(&pen->bg) : "",
    pen->bold ? "bold;" : "",
    pen->italic ? "italic;" : "",
    pen->underline ? "underline;" : "",
    pen->strikethrough ? "strikethrough;" : "",
    pen->blink ? "blink;" : "",
    pen->inverse ? "inverse;" : "",
    pen->link ? cli_term_link_to_string(term, pen->link) : ""
  );

  if (ret < 0) {
    R_THROW_POSIX_ERROR("Internal virtual terminal error");
  }

  return Rf_mkCharCE(buf, CE_UTF8);
}

int cli_term_pen_empty(struct pen *pen) {
  return
    pen->fg.col == 0 &&
    pen->bg.col == 0 &&
    !pen->bold &&
    !pen->italic &&
    !pen->underline &&
    !pen->strikethrough &&
    !pen->blink &&
    !pen->inverse &&
    !pen->link;
}

SEXP cli_term_links(struct terminal *term) {
  int i, n = term->linkptr;
  SEXP res = PROTECT(Rf_allocVector(VECSXP, n));
  for (i = 0; i < n; i++) {
    int start = term->links[i];
    int end = (i == n - 1) ? term->linkdataptr : term->links[i + 1];
    int len = end - start;
    SEXP elt = PROTECT(Rf_allocVector(INTSXP, len));
    memcpy(INTEGER(elt), term->linkdata + start, len * sizeof(CHARTYPE));
    SET_VECTOR_ELT(res, i, elt);
    UNPROTECT(1);
  }

  UNPROTECT(1);
  return res;
}

SEXP cli_term_state(struct terminal *term) {
  const char *res_names[] =
    { "lines", "attrs", "cursor_x", "cursor_y", "links", "" };
  SEXP res = PROTECT(Rf_mkNamed(VECSXP, res_names));
  SEXP lines = PROTECT(Rf_allocVector(VECSXP, term->height));
  SEXP attrs = PROTECT(Rf_allocVector(VECSXP, term->height));
  SET_VECTOR_ELT(res, 2, Rf_ScalarInteger(term->cursor_x));
  SET_VECTOR_ELT(res, 3, Rf_ScalarInteger(term->cursor_y));
  SET_VECTOR_ELT(res, 4, cli_term_links(term));
  int i, j, p;

  for (i = 0, p = 0; i < term->height; i++) {
    SEXP line = PROTECT(Rf_allocVector(INTSXP, term->width));
    SEXP attr = PROTECT(Rf_allocVector(STRSXP, term->width));
    for (j = 0; j < term->width; j++) {
      INTEGER(line)[j] = term->screen[p].ch;
      struct pen *current_pen = &term->screen[p].pen;
      if (!cli_term_pen_empty(current_pen)) {
        SET_STRING_ELT(attr, j, cli_term_pen_to_string(term, current_pen));
      }
      p++;
    }

    SET_VECTOR_ELT(lines, i, line);
    SET_VECTOR_ELT(attrs, i, attr);
    UNPROTECT(2);
  }

  SET_VECTOR_ELT(res, 0, lines);
  SET_VECTOR_ELT(res, 1, attrs);
  UNPROTECT(3);
  return res;
}

void cli_term_scroll_up(struct terminal *term) {
  memmove(
    term->screen,
    term->screen + term->width,
    term->width * (term->height - 1) * sizeof(struct cell)
  );
  cli_term_clear_line(term, term->height - 1);
}

void cli_term_move_cursor_down(struct terminal *term) {
  if (term->cursor_y == term->height - 1) {
    cli_term_scroll_up(term);
  } else {
    term->cursor_y += 1;
  }
  term->cursor_x = 0;
}

void cli_term_execute(struct terminal *term, int ch) {
  // TODO: rest
  switch (ch) {

  case '\n':
    cli_term_move_cursor_down(term);
    break;

  case '\r':
    term->cursor_x = 0;
    break;

  default:
    break;
  }
}

void cli_term_execute_sgr(vtparse_t *vt, struct terminal *term) {
  int i = 0, np = vt->num_params;
  while (i < np) {
    int param = vt->params[i];

    switch (param) {
    case 0:
      cli_term_reset_pen(&term->pen);
      i++;
      break;

    case 1:
      term->pen.bold = 1;
      i++;
      break;

    case 3:
      term->pen.italic = 1;
      i++;
      break;

    case 4:
      term->pen.underline = 1;
      i++;
      break;

    case 5:
      term->pen.blink = 1;
      i++;
      break;

    case 7:
      term->pen.inverse = 1;
      i++;
      break;

    case 9:
      term->pen.strikethrough = 1;
      i++;
      break;

    case 21:
      term->pen.bold = 0;
      i++;
      break;

    case 22:
      term->pen.bold = 0;
      i++;
      break;

    case 23:
      term->pen.italic = 0;
      i++;
      break;

    case 24:
      term->pen.underline = 0;
      i++;
      break;

    case 25:
      term->pen.blink = 0;
      i++;
      break;

    case 27:
      term->pen.inverse = 0;
      i++;
      break;

    case 30:
    case 31:
    case 32:
    case 33:
    case 34:
    case 35:
    case 36:
    case 37:
      term->pen.fg.col = param;
      i++;
      break;

    case 38:
      i++;
      if (i == np) break;
      if (vt->params[i] == 2) {
        i++;
        if (i + 2 < np) {
          term->pen.fg.col = CLI_COL_RGB;
          term->pen.fg.r = vt->params[i];
          term->pen.fg.g = vt->params[i + 1];
          term->pen.fg.b = vt->params[i + 2];
          i += 3;
        }
      } else if (vt->params[i] == 5) {
        i++;
        if (i < np) {
          term->pen.fg.col = CLI_COL_256;
          term->pen.fg.r = vt->params[i];
          i++;
        }
      } else {
        i++;
      }
      break;

    case 39:
      term->pen.fg.col = 0;
      i++;
      break;

    case 40:
    case 41:
    case 42:
    case 43:
    case 44:
    case 45:
    case 46:
    case 47:
      term->pen.bg.col = param;
      i++;
      break;

    case 48:
      i++;
      if (i == np) break;
      if (vt->params[i] == 2) {
        i++;
        if (i + 2 < np) {
          term->pen.bg.col = CLI_COL_RGB;
          term->pen.bg.r = vt->params[i];
          term->pen.bg.g = vt->params[i + 1];
          term->pen.bg.b = vt->params[i + 2];
          i += 3;
        }
      } else if (vt->params[i] == 5) {
        i++;
        if (i < np) {
          term->pen.bg.col = CLI_COL_256;
          term->pen.bg.r = vt->params[i];
          i++;
        }
      } else {
        i++;
      }
      break;

    case 49:
      term->pen.bg.col = 0;
      i++;
      break;

    default:
      i++;
      break;
    }
  }
}

void cli_term_csi_dispatch(vtparse_t *vt, struct terminal *term,
                           CHARTYPE ch) {
  // TODO: check intermediates for Dec stuff
  // TODO: rest
  switch (ch) {
  case 'm':
    cli_term_execute_sgr(vt, term);
    break;

  default:
    break;
  }
}

void cli_term_osc_end(struct terminal *term) {
  if (!term->osc) {
    R_THROW_ERROR("Internal vt error, OSC buffer not alloaced");
  }
  if (term->oscptr == 3 && term->osc[0] == '8' && term->osc[1] == ';' &&
      term->osc[2] == ';') {
    // closing hyperlinks are ESC ] 8 ; ;
    term->pen.link = 0;

  } else if (term->oscptr >= 2 && term->osc[0] == '8' && term->osc[1] == ';') {
    // opening hyperlinks are ESC ] 8 ; URL
    if (!term->links) {
      term->links = (int*) R_alloc(OSC_NUM_LINKS, sizeof(int));
      term->linkdata = (CHARTYPE*) R_alloc(OSC_LINK_DATA_LEN, sizeof(CHARTYPE));
    }
    if (term->linkptr == OSC_NUM_LINKS) {
      R_THROW_ERROR("Too many hyperlinks, internal vt limit in cli");
    }
    if (term->linkdataptr + term->oscptr - 2 > OSC_LINK_DATA_LEN) {
      R_THROW_ERROR("Too many, too long hyperlinks, internal vt limit in cli");
    }
    memcpy(
      term->linkdata + term->linkdataptr,
      term->osc + 2,
      (term->oscptr - 2) * sizeof(CHARTYPE)
    );
    term->links[term->linkptr] = term->linkdataptr;
    term->linkptr += 1;
    term->linkdataptr += (term->oscptr - 2);
    term->pen.link = term->linkptr; // We need a +1 here, 0 means no link
  }
}

void cli_term_osc_put(struct terminal *term, CHARTYPE ch) {
  if (!term->osc) {
    R_THROW_ERROR("Internal vt error, OSC buffer not alloaced");
  }
  if (term->oscptr == OSC_LEN) {
    R_THROW_ERROR("Internal vt error, OSC buffer is full");
  }
  term->osc[term->oscptr] = ch;
  term->oscptr += 1;
}

void cli_term_osc_start(struct terminal *term) {
  if (!term->osc) {
    term->osc = (CHARTYPE*) R_alloc(OSC_LEN, sizeof(CHARTYPE));
  }
  term->oscptr = 0;
}

void clic_vt_callback(vtparse_t *vt, vtparse_action_t action,
                      CHARTYPE ch) {

  struct terminal *term = (struct terminal*) vt->user_data;

  switch (action) {

  case VTPARSE_ACTION_CSI_DISPATCH:
    cli_term_csi_dispatch(vt, term, ch);
    break;

  case VTPARSE_ACTION_EXECUTE:
    cli_term_execute(term, ch);
    break;

  case VTPARSE_ACTION_OSC_END:
    cli_term_osc_end(term);
    break;

  case VTPARSE_ACTION_OSC_PUT:
    cli_term_osc_put(term, ch);
    break;

  case VTPARSE_ACTION_OSC_START:
    cli_term_osc_start(term);
    break;

  case VTPARSE_ACTION_PRINT:
    if (term->cursor_x == term->width) {
      if (term->cursor_y == term->height - 1) {
        cli_term_scroll_up(term);
      } else {
        term->cursor_y += 1;
      }
      term->cursor_x = 0;
    }
    term->screen[CUR(term)].ch = ch;
    term->screen[CUR(term)].pen = term->pen;
    term->cursor_x += 1;
    break;

  default:
    break;
  }
}

SEXP clic_vt_output(SEXP bytes, SEXP width, SEXP height) {
  int c_width = INTEGER(width)[0];
  int c_height = INTEGER(height)[0];

  vtparse_t vt;
  struct terminal term = { 0 };
  if (cli_term_init(&term, c_width, c_height)) {
    R_THROW_ERROR("Cannot initialize vittual terminal");
  }
  term.vt = &vt;

  vtparse_init(&vt, clic_vt_callback);
  vt.user_data = &term;
  vtparse(&vt, RAW(bytes), LENGTH(bytes));

  return cli_term_state(&term);
}
