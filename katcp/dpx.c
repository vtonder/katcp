#ifdef KATCP_EXPERIMENTAL

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include <sys/socket.h>
#include <sys/stat.h>

#include <arpa/inet.h>
#include <netinet/in.h>

#include <netc.h>
#include <katcp.h>
#include <katpriv.h>
#include <katcl.h>

#define MAP_UNSET               (-1)
#define MAP_INNER                 0
#define MAP_REMOTE                1

#define KATCP_MAP_FLAG_NONE       0
#define KATCP_MAP_FLAG_HIDDEN   0x1

#define KATCP_MAP_FLAG_REQUEST  0x10
#define KATCP_MAP_FLAG_INFORM   0x20
#define KATCP_MAP_FLAG_REPLY    0x40  /* should not be used */

#define FLAT_MAGIC 0x49021faf

#define FLAT_STATE_GONE          0
#define FLAT_STATE_CONNECTING    1
#define FLAT_STATE_UP            2
#define FLAT_STATE_PAUSE         3  /* suspend processing requests from fd */
/* WARNING: this might end up turning into pauses of different types: fd, msgqueue */
#define FLAT_STATE_DRAIN         4
#define FLAT_STATE_DEAD          5

/*******************************************************************************/

struct katcp_cmd_item{
  /* a single command */
  char *i_name;
  char *i_help;
  int (*i_call)(struct katcp_dispatch *d, int argc);
  unsigned int i_flags;
  char *i_data;
  void (*i_clear)(void *data);
};

#include <avltree.h>

struct katcp_cmd_map{
  /* "table" of commands */
  char *m_name;
  unsigned int m_refs;
  struct avl_tree *m_tree;
  struct katcp_cmd_item *m_fallback;
};

/********************************************************************/

void destroy_cmd_map(struct katcp_cmd_map *m);

/********************************************************************/

int init_flats_katcp(struct katcp_dispatch *d, unsigned int stories)
{
  struct katcp_shared *s;
  
  if(d == NULL){
    return -1;
  }

  s = d->d_shared;
  if(s == NULL){
    return -1;
  }

#ifdef KATCP_CONSISTENCY_CHECKS
  if(s->s_this){
    fprintf(stderr, "flat: initialising stack which already has a value\n");
    abort();
  }
#endif

  s->s_stories = 0;
  s->s_level = (-1);

  s->s_this = malloc(sizeof(struct katcp_flat *) * stories);
  if(s->s_this == NULL){
    return -1;
  }

  s->s_stories = stories;

  return 0;
}

void destroy_flats_katcp(struct katcp_dispatch *d)
{
  struct katcp_shared *s;
  
  if(d == NULL){
    return;
  }

  s = d->d_shared;
  if(s == NULL){
    return;
  }

  if(s->s_this){
    free(s->s_this);
    s->s_this = NULL;
  }

  s->s_level = (-1);
  s->s_stories = 0;
}


/********************************************************************/

void destroy_group_katcp(struct katcp_dispatch *d, struct katcp_group *g)
{
  struct katcp_shared *s;
  unsigned int i;

  s = d->d_shared;

  if(g == NULL){
    return;
  }

  /* WARNING: need to think about these tests */
  if(g->g_use > 0){
    g->g_use--;
    return;
  }

  /* could possibly leave out this test */
  if(g->g_count > 0){
    return; 
  }

  if(g->g_name){
    free(g->g_name);
    g->g_name = NULL;
  }

  if(g->g_inner_map){
    destroy_cmd_map(g->g_inner_map);
    g->g_inner_map = NULL;
  }

  if(g->g_remote_map){
    destroy_cmd_map(g->g_remote_map);
    g->g_remote_map = NULL;
  }

  if(g->g_flats){
    free(g->g_flats);
    g->g_flats = NULL;
  }

  for(i = 0; (i < s->s_members) && (g != s->s_groups[i]); i++);

  s->s_members--;
  if(i < s->s_members){
    s->s_groups[i] = s->s_groups[s->s_members];
  } else {
    if(i > s->s_members){
      s->s_members++;
    }
  }

  free(g);
}

struct katcp_group *create_group_katcp(struct katcp_dispatch *d, char *name)
{
  struct katcp_group *g, **tmp;
  struct katcp_shared *s;

  s = d->d_shared;

  g = malloc(sizeof(struct katcp_group));
  if(g == NULL){
    return NULL;
  }

  g->g_name = NULL;

  g->g_inner_map = NULL;
  g->g_remote_map = NULL;

  g->g_flats = NULL;
  g->g_count = 0;

  g->g_use = 0;

  tmp = realloc(s->s_groups, sizeof(struct katcp_group *) * (s->s_members + 1));
  if(tmp == NULL){
    destroy_group_katcp(d, g);
    return NULL;
  }

  s->s_groups = tmp;
  s->s_groups[s->s_members] = g;
  s->s_members++;

  /* s_fallback not set here, set up in default_ */

  if(name){
    g->g_name = strdup(name);
    if(g->g_name == NULL){
      destroy_group_katcp(d, g);
      return NULL;
    }
  }

  return g;
}

int hold_group_katcp(struct katcp_group *g)
{
  if(g == NULL){
    return -1;
  }

  return g->g_use++;
}

struct katcp_group *this_group_katcp(struct katcp_dispatch *d)
{
  /* WARNING: there may be cases when we are not in the context of a duplex/flat, but still within a group - this doesn't cater for such a case */
  struct katcp_shared *s;
  struct katcp_flat *f;

  s = d->d_shared;
  if(s == NULL){
    return NULL;
  }

  log_message_katcp(d, KATCP_LEVEL_TRACE, NULL, "duplex: stack is %u", s->s_level);

  if(s->s_level < 0){
#ifdef KATCP_CONSISTENCY_CHECKS
    fprintf(stderr, "flat: level is %d, negative\n", s->s_level);
#endif
    return NULL;
  }

  f = s->s_this[s->s_level];
#ifdef KATCP_CONSISTENCY_CHECKS
  if(f == NULL){
    fprintf(stderr, "flat: logic problem - should not encounter a null entry\n");
    abort();
    return NULL;
  }
#endif

  return f->f_group;
}

struct katcp_group *find_group_katcp(struct katcp_dispatch *d, char *name)
{
  struct katcp_shared *s;
  struct katcp_group *gx;
  unsigned int i;

  s = d->d_shared;
  if(s == NULL){
    return NULL;
  }

  if(name == NULL){
    /* WARNING: this could also be an error - helps detect usage problems */
    return s->s_fallback;
  }

  for(i = 0; i < s->s_members; i++){
    gx = s->s_groups[i];
    if(gx && gx->g_name){
      if(!strcmp(gx->g_name, name)){
        return gx;
      }
    }
  }

  return NULL;
}

/********************************************************************/

void destroy_cmd_item(struct katcp_cmd_item *i)
{
  if(i == NULL){
    return;
  }

  if(i->i_name){
    free(i->i_name);
    i->i_name = NULL;
  }

  if(i->i_help){
    free(i->i_help);
    i->i_help = NULL;
  }

  i->i_call = NULL;
  i->i_flags = 0;

  if(i->i_clear){
    (*(i->i_clear))(i->i_data);
    i->i_clear = NULL;
  }

  i->i_data = NULL;

  free(i);
}

void destroy_cmd_item_void(void *v)
{
  struct katcp_cmd_item *i;

  i = v;

  destroy_cmd_item(i);
}

void print_help_cmd_item(struct katcp_dispatch *d, char *key, void *v)
{
  struct katcp_cmd_item *i;

  i = v;

  if(i->i_flags & KATCP_MAP_FLAG_REQUEST){ 
    /* TODO: check more flags, eg if hidden */
    append_string_katcp(d, KATCP_FLAG_FIRST | KATCP_FLAG_STRING, KATCP_HELP_INFORM);
    append_string_katcp(d, KATCP_FLAG_STRING, i->i_name);
    append_string_katcp(d, KATCP_FLAG_LAST | KATCP_FLAG_STRING, i->i_help);
  }

}

struct katcp_cmd_item *create_cmd_item(char *name, char *help, unsigned int flags, int (*call)(struct katcp_dispatch *d, int argc), void *data, void (*clear)(void *data)){
  struct katcp_cmd_item *i;

  i = malloc(sizeof(struct katcp_cmd_item));
  if(i == NULL){
    return NULL;
  }

  i->i_name = NULL;
  i->i_help = NULL;

  i->i_flags = 0;

  i->i_call = NULL;
  i->i_data = NULL;
  i->i_clear = NULL;

  if(name){
    i->i_name = strdup(name);
    if(i->i_name == NULL){
      destroy_cmd_item(i);
      return NULL;
    }
  }

  if(help){
    i->i_help = strdup(help);
    if(i->i_help == NULL){
      destroy_cmd_item(i);
      return NULL;
    }
  }

  i->i_flags = flags;

  i->i_call = call;
  i->i_data = data;
  i->i_clear = clear;

  return i;
}

void destroy_cmd_map(struct katcp_cmd_map *m)
{
  if(m == NULL){
    return;
  }

  if(m->m_refs > 0){
    m->m_refs--;
  }

  if(m->m_refs > 0){
    return;
  }

  if(m->m_tree){
    destroy_avltree(m->m_tree, &destroy_cmd_item_void);
    m->m_tree = NULL;
  }

  if(m->m_fallback){
    destroy_cmd_item(m->m_fallback);
    m->m_fallback = NULL;
  }

  if(m->m_name){
    free(m->m_name);
    m->m_name = NULL;
  }
}

void hold_cmd_map(struct katcp_cmd_map *m)
{
  m->m_refs++;
}

struct katcp_cmd_map *create_cmd_map(char *name)
{
  struct katcp_cmd_map *m;

  m = malloc(sizeof(struct katcp_cmd_map));
  if(m == NULL){
    return NULL;
  }

  m->m_name = NULL;
  m->m_refs = 0;
  m->m_tree = NULL;
  m->m_fallback = NULL;

  m->m_tree = create_avltree();
  if(m->m_tree == NULL){
    destroy_cmd_map(m);
    return NULL;
  }

  if(name){
    m->m_name = strdup(name);
    if(m->m_name == NULL){
      destroy_cmd_map(m);
      return NULL;
    }
  }

  return m;
}

int add_full_cmd_map(struct katcp_cmd_map *m, char *name, char *help, unsigned int flags, int (*call)(struct katcp_dispatch *d, int argc), void *data, void (*clear)(void *data))
{
  struct katcp_cmd_item *i;
  struct avl_node *n;
  char *ptr;

  if(name == NULL){
    return -1;
  }

  switch(name[0]){
    case KATCP_REQUEST : 
    case KATCP_REPLY :
    case KATCP_INFORM : 
      ptr = name + 1;
      break;
    default: 
      ptr = name;
      break;
  }

  if(ptr[0] == '\0'){
    return -1;
  }

  i = create_cmd_item(ptr, help, flags, call, data, clear);
  if(i == NULL){
    return -1;
  }

  n = create_node_avltree(ptr, i);
  if(n == NULL){
    destroy_cmd_item(i);
    return -1;
  }
  
  if(add_node_avltree(m->m_tree, n) < 0){
    free_node_avltree(n, &destroy_cmd_item_void);
    return -1;
  }

  return 0;
}

int add_cmd_map(struct katcp_cmd_map *m, char *name, char *help, int (*call)(struct katcp_dispatch *d, int argc))
{
  return add_full_cmd_map(m, name, help, 0, call, NULL, NULL);
}

struct katcp_cmd_item *locate_cmd_item(struct katcp_flat *f, struct katcl_parse *p)
{
  struct katcp_cmd_item *i;
  struct katcp_cmd_map *m;
  char *str;

  str = get_string_parse_katcl(p, 0);
  if(str == NULL){
    return NULL;
  }

  if(str[0] == '\0'){
    return NULL;
  }

#if 1
  /* map not initialised */
  return NULL;
#endif

  i = find_data_avltree(m->m_tree, str + 1);
  if(i){
    return i;
  }

  return m->m_fallback;
}


/********************************************************************/

/* */

#ifdef KATCP_CONSISTENCY_CHECKS
static void sane_flat_katcp(struct katcp_flat *f)
{
  if(f == NULL){
    fprintf(stderr, "flat: received null pointer, expecting a flat structure\n");
    abort();
  }
  if(f->f_magic != FLAT_MAGIC){
    fprintf(stderr, "flat: bad magic 0x%x, expected 0x%x\n", f->f_magic, FLAT_MAGIC);
    abort();
  }
}
#else 
#define sane_flat_katcp(f);
#endif

static void deallocate_flat_katcp(struct katcp_dispatch *d, struct katcp_flat *f)
{
  sane_flat_katcp(f);

  /* TODO: make destruction an event ? */

  if(f->f_peer){
    /* WARNING: make sure we don't recurse on cleanup, invoking release callback */
    release_endpoint_katcp(d, f->f_peer);
    f->f_peer = NULL;
  }

  if(f->f_name){
    free(f->f_name);
    f->f_name = NULL;
  }

  if(f->f_line){
    destroy_katcl(f->f_line, 1);
    f->f_line = NULL;
  }

  /* TODO: will probably have to decouple ourselves here */
  f->f_shared = NULL;

#if 0
  if(f->f_backlog){
    destroy_queue_katcl(f->f_backlog);
    f->f_backlog = NULL;
  }
#endif

  if(f->f_rx){
#if 0
    /* destroy currently not needed, operate on assumption that it is transient ? */
    destroy_parse_katcl(f->f_rx);
#endif
    f->f_rx = NULL;
  }

  f->f_current_map = MAP_UNSET;

  if(f->f_inner_map){
    destroy_cmd_map(f->f_inner_map);
    f->f_inner_map = NULL;
  }
  if(f->f_remote_map){
    destroy_cmd_map(f->f_remote_map);
    f->f_remote_map = NULL;
  }

  f->f_group = NULL;

  f->f_magic = 0;

  free(f);
}

static void destroy_flat_katcp(struct katcp_dispatch *d, struct katcp_flat *f)
{
  struct katcp_group *gx;
  unsigned int i;

  sane_flat_katcp(f);

  /* TODO: make destruction an event ? */

  gx = f->f_group;

  if((gx == NULL) || (gx->g_count == 0)){
#ifdef KATCP_CONSISTENCY_CHECKS
    fprintf(stderr, "dpx: major logic problem: malformed or empty group at %p\n", gx);
    abort();
#endif
  } else {

    for(i = 0; (i < gx->g_count) && (gx->g_flats[i] != f); i++);

    gx->g_count--;

    if(i < gx->g_count){
      gx->g_flats[i] = gx->g_flats[gx->g_count];
    } else {
      if(i > gx->g_count){
        /* undo, we are in not found case */
        gx->g_count++;
      }
    }
  }

  deallocate_flat_katcp(d, f);
}

int wake_endpoint_flat_katcp(struct katcp_dispatch *d, struct katcp_endpoint *ep, struct katcp_message *msg, void *data)
{
  /* might end up setting different wake functions - some flats might not field requests */
  struct katcp_flat *f;

  f = data;

  sane_flat_katcp(f);

  log_message_katcp(d, KATCP_LEVEL_TRACE, NULL, "received message");

  /* TODO: lots more here */

  return KATCP_ENDPOINT_OK;
}

void release_endpoint_flat_katcp(struct katcp_dispatch *d, void *data)
{
  struct katcp_flat *f;

  f = data;

  sane_flat_katcp(f);

  f->f_peer = NULL;
}

struct katcp_flat *create_flat_katcp(struct katcp_dispatch *d, int fd, int up, char *name, struct katcp_group *g)
{
  /* TODO: what about cloning an existing one to preserve misc settings, including log level, etc */
  struct katcp_flat *f, **tmp;
  struct katcp_shared *s;
  struct katcp_group *gx;

  s = d->d_shared;

  if((s->s_members == 0) || (s->s_groups == NULL)){
    log_message_katcp(d, KATCP_LEVEL_ERROR, NULL, "no group to associate new connection with");
    return NULL;
  }

  gx = (g != NULL) ? g : s->s_fallback;

  tmp = realloc(gx->g_flats, sizeof(struct katcp_flat *) * (gx->g_count + 1));
  if(tmp == NULL){
    return NULL;
  }

  gx->g_flats = tmp;

  f = malloc(sizeof(struct katcp_flat));
  if(f == NULL){
    return NULL;
  }

  f->f_magic = FLAT_MAGIC;
  f->f_name = NULL;

  f->f_flags = 0;

  /* for cases where connect() still has to succeed */
  f->f_state = up ? FLAT_STATE_UP : FLAT_STATE_CONNECTING;
  f->f_exit_code = 0; /* WARNING: should technically be a fail, to catch cases where it isn't set at exit time */

  f->f_log_level = s->s_default;

  f->f_peer = NULL;
  f->f_line = NULL;
  f->f_shared = NULL;

#if 0
  f->f_backlog = NULL;
#endif
  f->f_rx = NULL;

  f->f_inner_map = NULL;
  f->f_remote_map = NULL;

  f->f_current_map = MAP_UNSET;

  f->f_group = NULL;

  if(name){
    f->f_name = strdup(name);
    if(f->f_name == NULL){
      destroy_flat_katcp(d, f);
      return NULL;
    }
  }

  f->f_peer = create_endpoint_katcp(d, &wake_endpoint_flat_katcp, &release_endpoint_flat_katcp, f);

  f->f_line = create_katcl(fd);
  if(f->f_line == NULL){
    destroy_flat_katcp(d, f);
    return NULL;
  }

#if 0
  f->f_backlog = create_queue_katcl();
  if(f->f_backlog == NULL){
    destroy_flat_katcp(d, f);
    return NULL;
  }
#endif

  f->f_shared = s;

  gx->g_flats[gx->g_count] = f;
  gx->g_count++;

  f->f_group = gx;

  hold_group_katcp(gx); 

  log_message_katcp(d, KATCP_LEVEL_DEBUG, NULL, "created instance for %s", name ? name : "<anonymous");

  return f;
}

struct katcp_flat *find_name_flat_katcp(struct katcp_dispatch *d, char *group, char *name)
{
  struct katcp_group *gx;
  struct katcp_flat *fx;
  struct katcp_shared *s;
  unsigned int i, j;

  s = d->d_shared;

  if(name == NULL){
    return NULL;
  }

  for(j = 0; j < s->s_members; j++){
    gx = s->s_groups[j];
    if((group == NULL) || (gx->g_name && (strcmp(group, gx->g_name) == 0))){
      for(i = 0; i < gx->g_count; i++){
        fx = gx->g_flats[i];
        if(fx->f_name && (strcmp(name, fx->f_name) == 0)){
          return fx;
        }
      }
    }
  }

  return NULL;
}

struct katcp_endpoint *endpoint_of_flat_katcp(struct katcp_dispatch *d, struct katcp_flat *fx)
{
  return fx->f_peer;
}

struct katcp_cmd_map *map_of_flat_katcp(struct katcp_flat *fx)
{
  struct katcp_group *gx;
  struct katcp_cmd_map *mx;

  if(fx == NULL){
    return NULL;
  }

  switch(fx->f_current_map){
    case MAP_INNER : 
      mx = fx->f_inner_map;
      break;
    case MAP_REMOTE : 
      mx = fx->f_remote_map;
      break;
    default : 
#ifdef KATCP_CONSISTENCY_CHECKS
      fprintf(stderr, "logic failure: searching for command without having a mode set\n");
      abort();
#endif
      return NULL;
  }

  if(mx){
    return mx;
  }

  gx = fx->f_group;
  if(gx == NULL){
    return NULL;
  }

  switch(fx->f_current_map){
    case MAP_INNER : 
      mx = gx->g_inner_map;
      break;
    case MAP_REMOTE : 
      mx = gx->g_remote_map;
      break;
    default : 
#ifdef KATCP_CONSISTENCY_CHECKS
      fprintf(stderr, "impossible logic failure: map mode changed underneath us");
      abort();
#endif
      return NULL;
  }

  return mx;
}

struct katcp_flat *this_flat_katcp(struct katcp_dispatch *d)
{
  struct katcp_shared *s;
  struct katcp_flat *f;

  s = d->d_shared;

  if(s == NULL){
    return NULL;
  }

  if(s->s_level < 0){
#ifdef KATCP_CONSISTENCY_CHECKS
    log_message_katcp(d, KATCP_LEVEL_TRACE, NULL, "duplex logic: level variable set to %d", s->s_level);
#endif
    return NULL;
  }

  f = s->s_this[s->s_level];
#ifdef KATCP_CONSISTENCY_CHECKS
  if(f == NULL){
    fprintf(stderr, "flat: logic problem - should not encounter a null entry\n");
    abort();
    return NULL;
  }
#endif

  return f;
}

int load_flat_katcp(struct katcp_dispatch *d)
{
  struct katcp_flat *f;
  struct katcp_shared *s;
  struct katcp_group *gx;
  unsigned int i, j, inc;
  int result, fd;

  s = d->d_shared;

  result = 0;

#ifdef DEBUG
  fprintf(stderr, "flat: loading %u units\n", s->s_floors);
#endif

  for(j = 0; j < s->s_members; j++){
    gx = s->s_groups[j];
    i = 0;
    while(i < gx->g_count){
      inc = 1;

      f = gx->g_flats[i];
      fd = fileno_katcl(f->f_line);

#if 0
      /* disabled: this log triggers io, which triggers this loop */
      log_message_katcp(d, KATCP_LEVEL_TRACE, NULL, "loading %p in state %d with fd %d", f, f->f_state, fd);
#endif

      switch(f->f_state){
        case FLAT_STATE_GONE :
#ifdef KATCP_CONSISTENCY_CHECKS
          fprintf(stderr, "flat: problem: state %u should have been removed already\n", f->f_state);
          abort();
#endif
          break;

        case FLAT_STATE_CONNECTING : 
          FD_SET(fd, &(s->s_write));
          if(fd > s->s_max){
            s->s_max = fd;
          }
          break;

        case FLAT_STATE_UP : 
          FD_SET(fd, &(s->s_read));
          if(fd > s->s_max){
            s->s_max = fd;
          }
          /* WARNING: fall */

        case FLAT_STATE_PAUSE :
        case FLAT_STATE_DRAIN :
          if(flushing_katcl(f->f_line)){
            FD_SET(fd, &(s->s_write));
            if(fd > s->s_max){
              s->s_max = fd;
            }
          } else {
            /* WARNING: risk is that errors/disconnects will go missing, maybe error fdset */
          }
          break;

        case FLAT_STATE_DEAD :

          gx->g_count--;
          if(i < gx->g_count){
            gx->g_flats[i] = gx->g_flats[gx->g_count];
          }
          deallocate_flat_katcp(d, f);

          inc = 0;

          break;

        default :
#ifdef KATCP_CONSISTENCY_CHECKS
          fprintf(stderr, "flat: problem: unknown state %u for %p\n", f->f_state, f);
          abort();
#endif
          break;

      }

      i += inc;
    }


    /* TODO: maybe do something if group is empty */
  }

  return result;
}

int run_flat_katcp(struct katcp_dispatch *d)
{
  struct katcp_flat *fx;
  struct katcp_shared *s;
  struct katcp_group *gx;
  struct katcp_cmd_map *mx;
  struct katcp_cmd_item *ix;
  unsigned int i, j, mask, len, nothing;
  int fd, result, r, argc, code, forget;
  char *str;

  s = d->d_shared;

  result = 0;

#ifdef DEBUG
  fprintf(stderr, "flat: running %u units\n", s->s_floors);
#endif

  for(j = 0; j < s->s_members; j++){
    gx = s->s_groups[j];
    for(i = 0; i < gx->g_count; i++){
      fx = gx->g_flats[i];

      /* WARNING: should be a proper push */
      s->s_this[0] = fx;
      s->s_level = 0;

      fd = fileno_katcl(fx->f_line);

      if(FD_ISSET(fd, &(s->s_write))){
        /* resume connect */
        if(fx->f_state == FLAT_STATE_CONNECTING){
          result = getsockopt(fd, SOL_SOCKET, SO_ERROR, &code, &len);
          if(result == 0){
            switch(code){
              case 0 :
                fx->f_state = FLAT_STATE_UP;
                log_message_katcp(d, KATCP_LEVEL_DEBUG, NULL, "async connect succeeded");
                break;
              case EINPROGRESS :
                log_message_katcp(d, KATCP_LEVEL_WARN, NULL, "saw an in progress despite write set being ready");
                break;
              default :
                log_message_katcp(d, KATCP_LEVEL_ERROR, NULL, "unable to connect: %s", strerror(code));
                fx->f_state = FLAT_STATE_DEAD;
                break;
            }
          }
        } else {
          /* flush messages */
          if(write_katcl(fx->f_line) < 0){
            fx->f_state = FLAT_STATE_DEAD;
          }
        }
      }

      if(FD_ISSET(fd, &(s->s_read))){
        /* acquire data */
        if(read_katcl(fx->f_line) < 0){
          fx->f_state = FLAT_STATE_DEAD;
        }
      }


      if(fx->f_state == FLAT_STATE_UP){

        fx->f_rx = ready_katcl(fx->f_line);
        if(fx->f_rx == NULL){
          result = parse_katcl(fx->f_line);
          if(result > 0){
            fx->f_rx = ready_katcl(fx->f_line);
          }
        }

        if(fx->f_rx){
          forget = 1;

          str = get_string_parse_katcl(fx->f_rx, 0);
          if(str){

            log_message_katcp(d, KATCP_LEVEL_TRACE, NULL, "received a message %s ...", str);
 
            switch(str[0]){
              case KATCP_REQUEST :
                mask = KATCP_MAP_FLAG_REQUEST;
                break;
              case KATCP_REPLY   :
                mask = KATCP_MAP_FLAG_REPLY;
                break;
              case KATCP_INFORM  :
                mask = KATCP_MAP_FLAG_INFORM;
                break;
              default :
                mask = 0; /* bit of an abuse ... */
                /* ignore malformed messages silently */
                break;
            }

            if(mask & (KATCP_MAP_FLAG_REQUEST | KATCP_MAP_FLAG_INFORM)){
              nothing = (mask & KATCP_MAP_FLAG_REQUEST) ? 1 : 0;
              fx->f_current_map = MAP_REMOTE;

              mx = map_of_flat_katcp(fx);
              if(mx){

                log_message_katcp(d, KATCP_LEVEL_TRACE, NULL, "attempting to match %s to tree", str + 1);

                ix = find_data_avltree(mx->m_tree, str + 1);
                if(ix){
                  if(mask & ix->i_flags){
                    log_message_katcp(d, KATCP_LEVEL_DEBUG, NULL, "found matching command of correct type");

                    if(ix->i_call){
                      argc = get_count_parse_katcl(fx->f_rx);

                      result = (*(ix->i_call))(d, argc);

                      log_message_katcp(d, KATCP_LEVEL_TRACE, NULL, "callback invocation returns %d", result);

                      switch(result){
                        /* TODO: pause, yield, etc, etc */
                        case KATCP_RESULT_FAIL : 
                        case KATCP_RESULT_INVALID : 
                        case KATCP_RESULT_OK : 
                          /* TODO: make sure we keep track of tags */
                          extra_response_katcl(fx->f_line, result, NULL);
                          nothing = 0;
                          break;
                        /* WARNING: unclear if we should support KATCP_RESULT_YIELD ? */
                        case KATCP_RESULT_PAUSE : 
                          fx->f_state = FLAT_STATE_PAUSE;
                          forget = 0;
                          break;
                      }

                    } else {
                      extra_response_katcl(fx->f_line, KATCP_RESULT_FAIL, "no callback available for trigger of %s", str);
                    }


                  } else {
                    log_message_katcp(d, (mask & KATCP_MAP_FLAG_REQUEST) ? KATCP_LEVEL_INFO : KATCP_LEVEL_DEBUG, NULL, "no type match for %s found", str + 1);
                  }
                } else {
                  log_message_katcp(d, (mask & KATCP_MAP_FLAG_REQUEST) ? KATCP_LEVEL_INFO : KATCP_LEVEL_DEBUG, NULL, "no match for %s found", str + 1);
                }
              } else {
                log_message_katcp(d, KATCP_LEVEL_WARN, NULL, "task %s not configured to process requests from input stream", fx->f_name);
              }

              fx->f_current_map = MAP_UNSET;
              if(nothing){
                extra_response_katcl(fx->f_line, KATCP_RESULT_FAIL, NULL);
              }
            }

            if(mask & KATCP_MAP_FLAG_REPLY){
              /* TODO: check if we have a request pending, collect result */
            }

          } /* else silently ignore where no or null initial parameter */

          if(forget){ /* make space for next message arrival */
            clear_katcl(fx->f_line);
            result = parse_katcl(fx->f_line);
            if(result > 0){ 
              mark_busy_katcp(d);
            }
          }
        }

      } /* end of STATE_UP work */


#if 0
      if(FD_ISSET(fd, &(s->s_read))){
        if(read_katcl(fx->f_line) < 0){
          /* FAIL somehow */
        }

        mx = map_of_flat_katcp(fx);
        if(mx == NULL){
          log_message_katcp(d, KATCP_LEVEL_ERROR, NULL, "no map to be found for client %s", fx->f_name);
          /* FAIL somehow */
        }


        while((r = parse_katcl(fx->f_line)) > 0){
          fx->f_rx = ready_katcl(fx->f_line);
#if 1
          if(fx->f_rx == NULL){
            log_message_katcp(d, KATCP_LEVEL_FATAL, NULL, "parse_katcl promised us data, but there isn't any");
            return -1;
          }
#endif

          str = get_string_parse_katcl(fx->f_rx, 0);
          if(str){
            switch(str[0]){
              case KATCP_REQUEST :
                mask = KATCP_MAP_FLAG_REQUEST;
                break;
              case KATCP_REPLY   :
                mask = KATCP_MAP_FLAG_REPLY;
                break;
              case KATCP_INFORM  :
                mask = KATCP_MAP_FLAG_INFORM;
                break;
              default :
                mask = 0; /* bit of an abuse ... */
                /* ignore malformed messages silently */
                break;
            }
            if(mask & (KATCP_MAP_FLAG_REQUEST | KATCP_MAP_FLAG_INFORM)){
              log_message_katcp(d, KATCP_LEVEL_TRACE, NULL, "attempting to match %s to tree", str + 1);

              ix = find_data_avltree(mx->m_tree, str + 1);
              if(ix){
                if(mask & ix->i_flags){
                  log_message_katcp(d, KATCP_LEVEL_DEBUG, NULL, "found matching command of correct type");

                  if(ix->i_call){
                    argc = get_count_parse_katcl(fx->f_rx);
                    result = (*(ix->i_call))(d, argc);

                    log_message_katcp(d, KATCP_LEVEL_TRACE, NULL, "callback invocation returns %d", result);

                    switch(result){
                      /* TODO: pause, yield, etc, etc */
                      case KATCP_RESULT_FAIL : 
                      case KATCP_RESULT_INVALID : 
                      case KATCP_RESULT_OK : 
                        /* TODO: make sure we keep track of tags */
                        extra_response_katcl(fx->f_line, result, NULL);
                        break;
                    }

                  } else {
                    extra_response_katcl(fx->f_line, KATCP_RESULT_FAIL, "unknown message");
                  }


                } else {
                  log_message_katcp(d, KATCP_LEVEL_DEBUG, NULL, "found match for %s, but wrong type");
                }
              } else {
                log_message_katcp(d, KATCP_LEVEL_DEBUG, NULL, "no match for %s found", str + 1);
              }
            }
          } /* else silently ignore */


        }

        if(r < 0){
          /* FAIL */
        }
      }
#endif


    }
  }

  /* WARNING: should be a pop */
  s->s_this[0] = NULL;
  s->s_level = (-1);

  return result;
}

/****************************************************************/

int accept_flat_katcp(struct katcp_dispatch *d, struct katcp_arb *a, unsigned int mode)
{
#define LABEL_BUFFER 32
  int fd, nfd;
  unsigned int len;
  struct sockaddr_in sa;
  struct katcp_flat *f;
  struct katcp_group *gx;
  char label[LABEL_BUFFER];
  long opts;

  fd = fileno_arb_katcp(d, a);

  if(!(mode & KATCP_ARB_READ)){
    log_message_katcp(d, KATCP_LEVEL_TRACE, NULL, "ay caramba, accept handler expected to be called with the read flag");
    return 0;
  }

  len = sizeof(struct sockaddr_in);
  nfd = accept(fd, (struct sockaddr *) &sa, &len);

  if(nfd < 0){
    log_message_katcp(d, KATCP_LEVEL_TRACE, NULL, "accept on %s failed: %s", name_arb_katcp(d, a), strerror(errno));
    return 0;
  }

  gx = data_arb_katcp(d, a);
  if(gx == NULL){
    log_message_katcp(d, KATCP_LEVEL_ERROR, NULL, "group needed for accept");
    close(fd);
    return 0;
  }

  opts = fcntl(nfd, F_GETFL, NULL);
  if(opts >= 0){
    opts = fcntl(nfd, F_SETFL, opts | O_NONBLOCK);
  }

  snprintf(label, LABEL_BUFFER, "%s:%d", inet_ntoa(sa.sin_addr), ntohs(sa.sin_port));
  label[LABEL_BUFFER - 1] = '\0';

  log_message_katcp(d, KATCP_LEVEL_INFO, NULL, "accepted new connection from %s via %s", label, name_arb_katcp(d, a));

  f = create_flat_katcp(d, nfd, 1, label, gx);
  if(f == NULL){
    close(nfd);
  }
  
  return 0;
#undef LABEL_BUFFER
}

struct katcp_arb *listen_flat_katcp(struct katcp_dispatch *d, char *name, struct katcp_group *g)
{
  int fd;
  struct katcp_arb *a;
  struct katcp_group *gx;
  long opts;
  struct katcp_shared *s;

  s = d->d_shared;

  gx = (g != NULL) ? g : s->s_fallback;

  if(gx == NULL){
    log_message_katcp(d, KATCP_LEVEL_ERROR, NULL, "no group given, no duplex instances can be created");
    return NULL;
  }

  fd = net_listen(name, 0, 0);
  if(fd < 0){
    log_message_katcp(d, KATCP_LEVEL_ERROR, NULL, "unable to listen on %s: %s", name, strerror(errno));
    return NULL;
  }

  opts = fcntl(fd, F_GETFL, NULL);
  if(opts >= 0){
    opts = fcntl(fd, F_SETFL, opts | O_NONBLOCK);
  }

  a = create_arb_katcp(d, name, fd, KATCP_ARB_READ, &accept_flat_katcp, gx);
  if(a == NULL){
    close(fd);
    return NULL;
  }

  hold_group_katcp(gx);

  return a;
}

/********************************************************************/

int help_group_cmd(struct katcp_dispatch *d, int argc)
{
  struct katcp_flat *fx;
  struct katcp_cmd_item *i;
  struct katcp_cmd_map *mx;
  char *name, *match;

  fx = this_flat_katcp(d);

  if(fx == NULL){
    log_message_katcp(d, KATCP_LEVEL_ERROR, NULL, "help group called outside expected path");
    return KATCP_RESULT_FAIL;
  }

  mx = map_of_flat_katcp(fx);
  if(mx == NULL){
    log_message_katcp(d, KATCP_LEVEL_ERROR, NULL, "no map to be found for client %s", fx->f_name);
    return KATCP_RESULT_FAIL;
  }

  name = arg_string_katcp(d, 1);
  if(name == NULL){
    log_message_katcp(d, KATCP_LEVEL_INFO, NULL, "should generate list of commands");
    if(mx->m_tree){
      print_inorder_avltree(d, mx->m_tree->t_root, &print_help_cmd_item, 0);
    }
  } else {
    switch(name[0]){
      case KATCP_REQUEST : 
      case KATCP_REPLY   :
      case KATCP_INFORM  :
        match = name + 1;
        break;
      default :
        match = name;
    }
    if(match[0] == '\0'){
      log_message_katcp(d, KATCP_LEVEL_ERROR, NULL, "unable to provide help on a null command");
      return KATCP_RESULT_FAIL;
    } else {
      i = find_data_avltree(mx->m_tree, match);
      if(i == NULL){
        log_message_katcp(d, KATCP_LEVEL_ERROR, NULL, "no match for %s found", name);
      } else {
        print_help_cmd_item(d, NULL, (void *)i);
      }
#if 0
      if(i->i_flags & KATCP_MAP_FLAG_REQUEST){
        log_message_katcp(d, KATCP_LEVEL_ERROR, NULL, "should print help message %s for %s", i->i_help, i->i_name);
      }
#endif
    }
  }

  return KATCP_RESULT_OK;
}

int setup_default_group(struct katcp_dispatch *d, char *name)
{
  struct katcp_group *gx;
  struct katcp_shared *s;
  struct katcp_cmd_map *m;

  s = d->d_shared;

  gx = create_group_katcp(d, name);
  if(gx == NULL){
    return -1;
  }

  /* WARNING: what about an inner map ? */
  if(gx->g_remote_map == NULL){
    m = create_cmd_map(name);
    if(m == NULL){
      destroy_group_katcp(d, gx);
      return -1;
    }

    gx->g_remote_map = m;
    hold_cmd_map(m);

    add_full_cmd_map(m, "help", "display help messages (?help [command])", KATCP_MAP_FLAG_REQUEST, &help_group_cmd, NULL, NULL);
  }

  if(s->s_fallback == NULL){
    hold_group_katcp(gx);
    s->s_fallback = gx;
  }

  return 0;
}

/********************************************************************/
/********************************************************************/
/********************************************************************/

int transmit_cmd_katcp(struct katcp_dispatch *d, int argc)
{
  struct katcp_shared *s;
  char *name, *group;
  struct katcp_flat *fx;
  struct katcl_parse *px;
  struct katcp_endpoint *source, *target;

  s = d->d_shared;

  if(s->s_fallback == NULL){
    if(setup_default_group(d, "default") < 0){
      return -1;
    }
  }

  name = arg_string_katcp(d, 1);
  if(name == NULL){
    return extra_response_katcp(d, KATCP_RESULT_INVALID, "usage");
  }

  if(argc > 2){
    group = arg_string_katcp(d, 2);
  } else {
    group = NULL;
  }

  fx = this_flat_katcp(d);
  if(fx == NULL){
    return extra_response_katcp(d, KATCP_RESULT_FAIL, "cmd not run within a session");
  }
  source = endpoint_of_flat_katcp(d, fx);

  fx = find_name_flat_katcp(d, group, name);
  if(fx == NULL){
    return extra_response_katcp(d, KATCP_RESULT_FAIL, "no peer of name %s", name);
  }
  target = endpoint_of_flat_katcp(d, fx);

  log_message_katcp(d, KATCP_LEVEL_TRACE, NULL, "sending ping from endpoint %p to endpoint %p", source, target);

  px = create_parse_katcl();
  if(px == NULL){
    return extra_response_katcp(d, KATCP_RESULT_FAIL, "allocation");
  }

  add_string_parse_katcl(px, KATCP_FLAG_FIRST | KATCP_FLAG_LAST | KATCP_FLAG_STRING, "?watchdog");


  if(send_message_endpoint_katcp(d, source, target, px, 1) < 0){
    return KATCP_RESULT_FAIL;
  }

  return KATCP_RESULT_OK;
}

int listen_duplex_cmd_katcp(struct katcp_dispatch *d, int argc)
{
  char *name, *group;
  struct katcp_group *gx;
  struct katcp_shared *s;

  s = d->d_shared;

  if(s->s_fallback == NULL){
    if(setup_default_group(d, "default") < 0){
      return -1;
    }
  }

  name = arg_string_katcp(d, 1);
  if(name == NULL){
    return extra_response_katcp(d, KATCP_RESULT_INVALID, "usage");
  }

  group = arg_string_katcp(d, 2);
  if(group == NULL){
    gx = s->s_fallback;
  } else {
    gx = find_group_katcp(d, group);
  }

  if(gx == NULL){
    log_message_katcp(d, KATCP_LEVEL_ERROR, NULL, "unable to find a group to associate with listen on %s", name);
    return KATCP_RESULT_FAIL;
  }

  if(listen_flat_katcp(d, name, gx) == NULL){
    log_message_katcp(d, KATCP_LEVEL_ERROR, NULL, "unable to listen on %s: %s", name, strerror(errno));
    return KATCP_RESULT_FAIL;
  }

  return KATCP_RESULT_OK;
}

int list_duplex_cmd_katcp(struct katcp_dispatch *d, int argc)
{
  unsigned int i, j;
  struct katcp_shared *s;
  struct katcp_group *gx;
  struct katcp_flat *fx;

  s = d->d_shared;

  for(j = 0; j < s->s_members; j++){
    gx = s->s_groups[j];
    log_message_katcp(d, KATCP_LEVEL_INFO, NULL, "group[%d]=%p is %s", j, gx, gx->g_name ? gx->g_name : "<anonymous>");
    log_message_katcp(d, KATCP_LEVEL_INFO, NULL, "group[%d]=%p has %d references and %u members", j, gx, gx->g_use, gx->g_count);
    for(i = 0; i < gx->g_count; i++){
      fx = gx->g_flats[i];
      log_message_katcp(d, KATCP_LEVEL_INFO, NULL, "%s at %p in state %u", fx->f_name, fx, fx->f_state);
#if 0
      log_message_katcp(d, KATCP_LEVEL_INFO, NULL, "%s has queue backlog of %u", fx->f_name, size_queue_katcl(fx->f_backlog));
#endif
      log_message_katcp(d, KATCP_LEVEL_INFO, NULL, "%s is part of group %p", fx->f_name, fx->f_group);
    }
  }

  return KATCP_RESULT_OK;
}

#endif
