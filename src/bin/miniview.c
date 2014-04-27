#include <Elementary.h>
#include <stdio.h>
#include <assert.h>

#include "col.h"
#include "termpty.h"
#include "termio.h"
#include "main.h"

/* specific log domain to help debug only miniview */
int _miniview_log_dom = -1;

#undef CRITICAL
#undef ERR
#undef WRN
#undef INF
#undef DBG

#define CRIT(...)     EINA_LOG_DOM_CRIT(_miniview_log_dom, __VA_ARGS__)
#define ERR(...)      EINA_LOG_DOM_ERR(_miniview_log_dom, __VA_ARGS__)
#define WRN(...)      EINA_LOG_DOM_WARN(_miniview_log_dom, __VA_ARGS__)
#define INF(...)      EINA_LOG_DOM_INFO(_miniview_log_dom, __VA_ARGS__)
#define DBG(...)      EINA_LOG_DOM_DBG(_miniview_log_dom, __VA_ARGS__)

/*TODO: work with splits */

void
miniview_init(void)
{
   if (_miniview_log_dom >= 0) return;

   _miniview_log_dom = eina_log_domain_register("miniview", NULL);
   if (_miniview_log_dom < 0)
     EINA_LOG_CRIT("could not create log domain 'miniview'.");
}

void
miniview_shutdown(void)
{
   if (_miniview_log_dom < 0) return;
   eina_log_domain_unregister(_miniview_log_dom);
   _miniview_log_dom = -1;
}

typedef struct _Miniview Miniview;

struct _Miniview
{
   Evas_Object *self;
   Evas_Object *img;
   Evas_Object *termio;
   Termpty *pty;

   int img_h;
   int img_hist; /* history rendered is between img_hpos(<0) and
                    img_pos - image_height */
   int img_off; /* >0; offset for the visible part */
   int viewport_h;
   int rows;
};

static Evas_Smart *_smart = NULL;

static void
_scroll(Miniview *mv, int z)
{
   int history_len = mv->pty->backscroll_num;
   int new_offset;

   /* whether to move img or modify it */
   DBG("history_len:%d z:%d img:h:%d hist:%d off:%d viewport:%d",
       history_len, z, mv->img_h, mv->img_hist, mv->img_off,mv->viewport_h);
   /* top? */
   if ((mv->img_hist == -history_len && mv->img_off == 0  && z < 0))
     {
        DBG("TOP");
        return;
     }
   /* bottom? */
   if (mv->img_hist + mv->viewport_h + mv->img_off >= mv->rows && z > 0) /* bottom */
     {
        DBG("BOTTOM");
        return;
     }

   new_offset = mv->img_off + z;

   if (new_offset >= 0 &&
       new_offset + mv->viewport_h < mv->img_h)
     {
        /* move */
        Evas_Coord ox, oy;

        /* TODO: boundaries */

        mv->img_off = new_offset;
        evas_object_geometry_get(mv->img, &ox, &oy, NULL, NULL);
        evas_object_move(mv->img, ox, oy - z);
     }
   else
     {
        /* TODO: boris */
        DBG("TODO: redraw");
     }
}

static void
_smart_cb_mouse_wheel(void *data, Evas *e EINA_UNUSED,
                      Evas_Object *obj EINA_UNUSED, void *event)
{
   Evas_Event_Mouse_Wheel *ev = event;
   Miniview *mv = evas_object_smart_data_get(data);

   EINA_SAFETY_ON_NULL_RETURN(mv);

   /* do not handle horizontal scrolling */
   if (ev->direction) return;

   DBG("ev->z:%d", ev->z);

   _scroll(mv, ev->z * 10);
}


static void
_smart_add(Evas_Object *obj)
{
   Miniview *mv;
   Evas_Object *o;
   Evas *canvas = evas_object_evas_get(obj);

   DBG("%p", obj);

   mv = calloc(1, sizeof(Miniview));
   EINA_SAFETY_ON_NULL_RETURN(mv);
   evas_object_smart_data_set(obj, mv);

   mv->self = obj;

   /* miniview output widget  */
   o = evas_object_image_add(canvas);
   evas_object_image_alpha_set(o, EINA_TRUE);

   evas_object_smart_member_add(o, obj);
   mv->img = o;

   /* TODO: see if we can use an elm scroller */
   evas_object_event_callback_add(o, EVAS_CALLBACK_MOUSE_WHEEL,
                                  _smart_cb_mouse_wheel, obj);
}

static void
_smart_del(Evas_Object *obj)
{
   Miniview *mv = evas_object_smart_data_get(obj);

   DBG("%p", obj);
   if (!mv) return;
   /* TODO */
   DBG("%p", obj);
}

static void
_smart_move(Evas_Object *obj, Evas_Coord x EINA_UNUSED, Evas_Coord y EINA_UNUSED)
{
   Miniview *mv = evas_object_smart_data_get(obj);

   if (!mv) return;
   /* TODO */
   DBG("%p x:%d y:%d", obj, x, y);
   evas_object_move(mv->img, x, y);
}

static void
_draw_line(unsigned int *pixels, Termcell *cells, int length)
{
   int x;

   for (x = 0 ; x < length; x++)
     {
        int r, g, b;

        if (cells[x].codepoint > 0 && !isspace(cells[x].codepoint) &&
            !cells[x].att.newline && !cells[x].att.tab &&
            !cells[x].att.invisible && cells[x].att.bg != COL_INVIS)
          {
             switch (cells[x].att.fg)
               {
                  // TODO: get pixel colors from current themee...
                case 0:
                   r = 180; g = 180; b = 180;
                   break;
                case 2:
                   r = 204; g = 51; b = 51;
                   break;
                case 3:
                   r = 51; g = 204; b = 51;
                   break;
                case 4:
                   r = 204; g = 136; b = 51;
                   break;
                case 5:
                   r = 51; g = 51; b = 204;
                   break;
                case 6:
                   r = 204; g = 51; b = 204;
                   break;
                case 7:
                   r = 51; g = 204; b = 204;
                   break;
                default:
                   r = 180; g = 180; b = 180;
               }
             pixels[x] = (0xff << 24) | (r << 16) | (g << 8) | b;
          }
     }
}

static void
_smart_show(Evas_Object *obj)
{
   Miniview *mv = evas_object_smart_data_get(obj);

   if (!mv) return;

   /*
   Evas_Coord ox, oy, ow, oh;
   evas_object_geometry_get(mv->img, &ox, &oy, &ow, &oh);
   DBG("ox:%d oy:%d ow:%d oh:%d visible:%d|%d %d %d %d",
       ox, oy, ow, oh,
       evas_object_visible_get(obj),
       evas_object_visible_get(mv->img),
       evas_object_layer_get(mv->img),
       evas_object_layer_get(obj),
       evas_object_layer_get(mv->termio));
       */

   evas_object_show(mv->img);
}

static void
_smart_hide(Evas_Object *obj)
{
   Miniview *mv = evas_object_smart_data_get(obj);

   if (!mv) return;

   evas_object_hide(mv->img);
}

static void
_smart_size(Evas_Object *obj)
{
   Miniview *mv = evas_object_smart_data_get(obj);
   Evas_Coord ox, oy, ow, oh, font_w, font_h;
   int history_len, columns, h, y, wret;
   unsigned int *pixels;
   Termcell *cells;


   if (!mv) return;

   DBG("smart size %p", obj);

   evas_object_geometry_get(mv->termio, &ox, &oy, &ow, &oh);
   if (ow == 0 || oh == 0) return;
   evas_object_size_hint_min_get(mv->termio, &font_w, &font_h);

   if (font_w <= 0) return;

   columns = ow / font_w;
   mv->rows = oh / font_h;
   mv->img_h = 3 * oh;

   DBG("ox:%d oy:%d ow:%d oh:%d font_w:%d columns:%d",
       ox, oy, ow, oh, font_w, columns);

   evas_object_resize(mv->img, columns, mv->img_h);
   evas_object_image_size_set(mv->img, columns, mv->img_h);

   evas_object_image_fill_set(mv->img, 0, 0, columns, mv->img_h);

   history_len = mv->pty->backscroll_num;

   pixels = evas_object_image_data_get(mv->img, EINA_TRUE);
   memset(pixels, 0, sizeof(*pixels) * columns * mv->img_h);

   mv->viewport_h = oh;
   h = mv->img_h - mv->rows;
   if (h < history_len)
     {
        mv->img_hist = mv->rows - mv->img_h;
     }
   else
     {
        mv->img_hist = -history_len;
     }

   DBG("img_h:%d history_len:%d h:%d img_hist:%d vph:%d",
       mv->img_h, history_len, h, mv->img_hist, mv->viewport_h);

   for (y = 0; y < mv->img_h; y++)
     {
        cells = termpty_cellrow_get(mv->pty, mv->img_hist + y, &wret);
        if (cells == NULL)
          {
             DBG("y:%d get:%d", y, mv->img_hist + y);
          break;
          }

        _draw_line(&pixels[y*columns], cells, wret);
     }


   if (y > mv->viewport_h)
     {
        mv->img_off = y - mv->viewport_h;
        evas_object_move(mv->img,
                         ox + ow - columns,
                         - mv->img_off);
     }
   else
     {
        mv->img_off = 0;
        evas_object_move(mv->img,
                         ox + ow - columns,
                         0);
     }

   DBG("history_len:%d img:h:%d hist:%d off:%d viewport:%d",
       history_len, mv->img_h, mv->img_hist, mv->img_off,mv->viewport_h);
}





static void
_smart_resize(Evas_Object *obj, Evas_Coord w, Evas_Coord h)
{
   Miniview *mv = evas_object_smart_data_get(obj);
   if (!mv) return;

   DBG("smart resize %p w:%d h:%d", obj, w, h);
   evas_object_resize(mv->img, w, h);
   _smart_size(obj);
}


static void
_smart_init(void)
{
    static Evas_Smart_Class sc = EVAS_SMART_CLASS_INIT_NULL;

    DBG("smart init");

    sc.name      = "miniview";
    sc.version   = EVAS_SMART_CLASS_VERSION;
    sc.add       = _smart_add;
    sc.del       = _smart_del;
    sc.resize    = _smart_resize;
    sc.move      = _smart_move;
    sc.show      = _smart_show;
    sc.hide      = _smart_hide;
    _smart = evas_smart_class_new(&sc);
}


void
miniview_update_scroll(Evas_Object *obj, int scroll_position)
{
   Miniview *mv = evas_object_smart_data_get(obj);
   if (!mv) return;

   DBG("obj:%p mv:%p scroll_position:%d", obj, mv, scroll_position);
}

Evas_Object *
miniview_add(Evas_Object *parent, Evas_Object *termio)
{
   Evas *e;
   Evas_Object *obj;
   Miniview *mv;

   EINA_SAFETY_ON_NULL_RETURN_VAL(parent, NULL);
   e = evas_object_evas_get(parent);
   if (!e) return NULL;

   DBG("ADD parent:%p", parent);

   if (!_smart) _smart_init();

   obj = evas_object_smart_add(e, _smart);
   mv = evas_object_smart_data_get(obj);
   if (!mv) return obj;

   mv->termio = termio;
   mv->pty = termio_pty_get(termio);

   _smart_size(obj);

   return obj;
}

