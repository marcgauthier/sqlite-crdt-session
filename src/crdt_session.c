/* SQLite Session-backed, cell-level last-writer-wins CRDT prototype.
 * All data and CRDT metadata are stored in the SQLite main database.
 * Requires SQLite built with SQLITE_ENABLE_SESSION and SQLITE_ENABLE_PREUPDATE_HOOK.
 * Explicit capture/flush is required around every local write transaction.
 */
#define _POSIX_C_SOURCE 200809L
#define SQLITE_ENABLE_SESSION 1
#define SQLITE_ENABLE_PREUPDATE_HOOK 1
#include <sqlite3ext.h>
SQLITE_EXTENSION_INIT1
#include <sqlite3.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>

#define ROW_CID "__crdt_row"
#define DELETE_CID "__crdt_delete"
#define EPOCH_JULIAN_MS 210866760000000LL

typedef struct CrdtContext {
  sqlite3 *db;
  sqlite3_session *session;
  sqlite3_int64 last_clock;
  sqlite3_int64 local_seq;
  int initialized;
} CrdtContext;

typedef struct TableInfo {
  char *name;
  char **columns;
  int ncols;
  int *pk_index;
  int npk;
} TableInfo;

typedef struct KeyBuffer { unsigned char *ptr; int len; int cap; } KeyBuffer;

static void destroy_context(void *ptr) {
  CrdtContext *c=(CrdtContext*)ptr;
  if(c) { if(c->session) sqlite3session_delete(c->session); sqlite3_free(c); }
}
static void free_table(TableInfo *t) {
  if(t->columns) { for(int i=0;i<t->ncols;i++) sqlite3_free(t->columns[i]); sqlite3_free(t->columns); }
  sqlite3_free(t->pk_index); sqlite3_free(t->name); memset(t,0,sizeof(*t));
}
static char *quote_ident(const char *s) {
  /* %w doubles quote characters inside identifiers. */
  if(!s || !*s) return NULL;
  return sqlite3_mprintf("\"%w\"",s);
}
static int run_sql(sqlite3 *db, const char *sql) { return sqlite3_exec(db,sql,NULL,NULL,NULL); }
static int prepare(sqlite3 *db,sqlite3_stmt **s,const char *sql) {
  return sqlite3_prepare_v2(db,sql,-1,s,NULL);
}
static int exec_stmt(sqlite3_stmt *s) {
  int rc=sqlite3_step(s); return rc==SQLITE_DONE ? SQLITE_OK : rc;
}
static void err_sql(sqlite3_context *ctx,int rc) {
  sqlite3_result_error(ctx,sqlite3_errmsg(sqlite3_context_db_handle(ctx)),-1);
  sqlite3_result_error_code(ctx,rc);
}
static int get_config_i64(sqlite3 *db,const char *name,sqlite3_int64 *out) {
  sqlite3_stmt *s=NULL; int rc=prepare(db,&s,"SELECT CAST(value AS INTEGER) FROM _crdt_config WHERE key=?1");
  if(rc==SQLITE_OK){sqlite3_bind_text(s,1,name,-1,SQLITE_STATIC);rc=sqlite3_step(s);
    if(rc==SQLITE_ROW){*out=sqlite3_column_int64(s,0);rc=SQLITE_OK;}
    else if(rc==SQLITE_DONE){*out=0;rc=SQLITE_OK;}}
  sqlite3_finalize(s);return rc;
}
static int set_config_i64(sqlite3 *db,const char *name,sqlite3_int64 value) {
  sqlite3_stmt *s=NULL;int rc=prepare(db,&s,
    "INSERT INTO _crdt_config(key,value) VALUES(?1,?2) ON CONFLICT(key) DO UPDATE SET value=excluded.value");
  if(rc==SQLITE_OK){sqlite3_bind_text(s,1,name,-1,SQLITE_STATIC);sqlite3_bind_int64(s,2,value);rc=exec_stmt(s);}
  sqlite3_finalize(s);return rc;
}
static int get_node(sqlite3 *db,char **name) {
  *name=NULL;sqlite3_stmt *s=NULL;int rc=prepare(db,&s,"SELECT value FROM _crdt_config WHERE key='site_id'");
  if(rc==SQLITE_OK){rc=sqlite3_step(s);if(rc==SQLITE_ROW){
      const unsigned char *z=sqlite3_column_text(s,0);
      *name=sqlite3_mprintf("%s",z ? (const char*)z : "");rc=*name?SQLITE_OK:SQLITE_NOMEM;
    }else if(rc==SQLITE_DONE)rc=SQLITE_NOTFOUND;}
  sqlite3_finalize(s);return rc;
}
static int require_initialized(sqlite3_context *ctx) {
  char *n=NULL;int rc=get_node(sqlite3_context_db_handle(ctx),&n);sqlite3_free(n);
  if(rc!=SQLITE_OK)sqlite3_result_error(ctx,"call crdt_init(site_id) first",-1);
  return rc;
}
static sqlite3_int64 now_ms(sqlite3 *db) {
  sqlite3_vfs *v=sqlite3_vfs_find(NULL);sqlite3_int64 jd=0;
  if(v && v->iVersion>=2 && v->xCurrentTimeInt64 && v->xCurrentTimeInt64(v,&jd)==SQLITE_OK)
    return jd-EPOCH_JULIAN_MS;
  (void)db;return 0;
}
static sqlite3_int64 tick(sqlite3 *db,sqlite3_int64 last) {
  sqlite3_int64 physical=now_ms(db);
  sqlite3_int64 old_millis=last>>16;
  if(physical>old_millis) return physical<<16;
  if((last & 0xffffLL)!=0xffffLL) return last+1;
  return (old_millis+1)<<16;
}
static int version_wins(sqlite3_int64 hlc,const char *site,
                        sqlite3_int64 old_clock,const char *old_site) {
  if(hlc!=old_clock) return hlc>old_clock;
  return strcmp(site,old_site)>0;
}
static int read_version(sqlite3 *db,const char *table,const void *pk,int npk,
                        const char *cid,sqlite3_int64 *clock,char **node) {
  *clock=0;*node=NULL;
  sqlite3_stmt *s=NULL;int rc=prepare(db,&s,
    "SELECT hlc,site_id FROM _crdt_cells WHERE table_name=?1 AND pk=?2 AND cid=?3");
  if(rc==SQLITE_OK) {
    sqlite3_bind_text(s,1,table,-1,SQLITE_TRANSIENT);
    sqlite3_bind_blob(s,2,pk,npk,SQLITE_TRANSIENT);
    sqlite3_bind_text(s,3,cid,-1,SQLITE_TRANSIENT);
    rc=sqlite3_step(s);
    if(rc==SQLITE_ROW){*clock=sqlite3_column_int64(s,0);
      *node=sqlite3_mprintf("%s",sqlite3_column_text(s,1));rc=*node?SQLITE_OK:SQLITE_NOMEM;
    }else if(rc==SQLITE_DONE)rc=SQLITE_OK;
  }
  sqlite3_finalize(s);return rc;
}
static int write_cell(sqlite3 *db,const char *table,const void *pk,int npk,const char *cid,
                       sqlite3_int64 hlc,const char *site) {
  sqlite3_stmt *s=NULL;int rc=prepare(db,&s,
    "INSERT INTO _crdt_cells(table_name,pk,cid,hlc,site_id) VALUES(?1,?2,?3,?4,?5) "
    "ON CONFLICT(table_name,pk,cid) DO UPDATE SET hlc=excluded.hlc,site_id=excluded.site_id");
  if(rc==SQLITE_OK){sqlite3_bind_text(s,1,table,-1,SQLITE_TRANSIENT);
    sqlite3_bind_blob(s,2,pk,npk,SQLITE_TRANSIENT);
    sqlite3_bind_text(s,3,cid,-1,SQLITE_TRANSIENT);
    sqlite3_bind_int64(s,4,hlc);sqlite3_bind_text(s,5,site,-1,SQLITE_TRANSIENT);
    rc=exec_stmt(s);}
  sqlite3_finalize(s);return rc;
}
static int append_log(sqlite3 *db,const char *table,const void *pk,int npk,const char *cid,
                      sqlite3_value *value,sqlite3_int64 hlc,const char *site,sqlite3_int64 seq) {
  sqlite3_stmt *s=NULL;int rc=prepare(db,&s,
    "INSERT INTO _crdt_log(site_id,seq,table_name,pk,cid,val,hlc) VALUES(?1,?2,?3,?4,?5,?6,?7)");
  if(rc==SQLITE_OK){
    sqlite3_bind_text(s,1,site,-1,SQLITE_TRANSIENT);sqlite3_bind_int64(s,2,seq);
    sqlite3_bind_text(s,3,table,-1,SQLITE_TRANSIENT);sqlite3_bind_blob(s,4,pk,npk,SQLITE_TRANSIENT);
    sqlite3_bind_text(s,5,cid,-1,SQLITE_TRANSIENT);
    if(value)sqlite3_bind_value(s,6,value);else sqlite3_bind_null(s,6);
    sqlite3_bind_int64(s,7,hlc);rc=exec_stmt(s);
  }
  sqlite3_finalize(s);return rc;
}
static int kb_add(KeyBuffer *b,const void *ptr,int n) {
  if(n<0 || b->len>INT_MAX-n) return SQLITE_TOOBIG;
  if(b->len+n>b->cap){int cap=b->cap?b->cap:64;while(cap<b->len+n){
      if(cap>INT_MAX/2){cap=b->len+n;break;}cap*=2;}
    void *p=sqlite3_realloc64(b->ptr,(sqlite3_uint64)cap);if(!p)return SQLITE_NOMEM;
    b->ptr=(unsigned char*)p;b->cap=cap;
  }
  if(n)memcpy(b->ptr+b->len,ptr,(size_t)n);
  b->len+=n;return SQLITE_OK;
}
static int kb_u32(KeyBuffer *b,uint32_t x) {
  unsigned char v[4]={(unsigned char)(x>>24),(unsigned char)(x>>16),(unsigned char)(x>>8),(unsigned char)x};
  return kb_add(b,v,4);
}
static int kb_value(KeyBuffer *b,sqlite3_value *v){
  if(!v || sqlite3_value_type(v)==SQLITE_NULL)return SQLITE_MISMATCH;
  int t=sqlite3_value_type(v),rc;
  unsigned char kind=(unsigned char)(t==SQLITE_INTEGER?'I':t==SQLITE_FLOAT?'R':t==SQLITE_TEXT?'T':'B');
  rc=kb_add(b,&kind,1);if(rc!=SQLITE_OK)return rc;
  if(t==SQLITE_INTEGER || t==SQLITE_FLOAT){uint64_t x=0;
    if(t==SQLITE_INTEGER)x=(uint64_t)sqlite3_value_int64(v);
    else {double d=sqlite3_value_double(v);memcpy(&x,&d,8);}
    unsigned char octets[8];for(int j=7;j>=0;j--){octets[j]=(unsigned char)x;x>>=8;}
    return kb_add(b,octets,8);
  }
  const void *ptr=t==SQLITE_TEXT?(const void*)sqlite3_value_text(v):sqlite3_value_blob(v);
  int n=sqlite3_value_bytes(v);if(n<0)return SQLITE_ERROR;
  if((rc=kb_u32(b,(uint32_t)n))!=SQLITE_OK)return rc;
  return kb_add(b,ptr,n);
}
static int make_key(sqlite3_changeset_iter *it,int op,const TableInfo *tab,
                    const unsigned char *pk_mask,KeyBuffer *buf){
  int rc=SQLITE_OK;
  for(int i=0;i<tab->npk;i++){
    int col=tab->pk_index[i];sqlite3_value *v=NULL;
    if(!pk_mask[col]) return SQLITE_SCHEMA;
    rc=op==SQLITE_INSERT?sqlite3changeset_new(it,col,&v):sqlite3changeset_old(it,col,&v);
    if(rc!=SQLITE_OK)return rc;
    if(!v && op==SQLITE_UPDATE)rc=sqlite3changeset_new(it,col,&v);
    if(rc!=SQLITE_OK)return rc;
    if((rc=kb_value(buf,v))!=SQLITE_OK)return rc;
  }
  return SQLITE_OK;
}
static int load_table(sqlite3 *db,const char *name,TableInfo *t) {
  memset(t,0,sizeof(*t));int rc=SQLITE_OK;
  if(!name || strncmp(name,"sqlite_",7)==0 || strncmp(name,"_crdt_",6)==0 ||
      strcmp(name,"crdt_changes")==0 || strcmp(name,"crsql_changes")==0) return SQLITE_MISUSE;
  sqlite3_stmt *check=NULL;
  rc=prepare(db,&check,"SELECT type,sql FROM sqlite_master WHERE name=?1 AND type='table'");
  if(rc!=SQLITE_OK)return rc;
  sqlite3_bind_text(check,1,name,-1,SQLITE_TRANSIENT);
  rc=sqlite3_step(check);
  if(rc!=SQLITE_ROW){sqlite3_finalize(check);return SQLITE_NOTFOUND;}
  const unsigned char *sql=sqlite3_column_text(check,1);
  if(sql && (strstr((const char*)sql,"CREATE VIRTUAL TABLE") || strstr((const char*)sql,"create virtual table"))) {
    sqlite3_finalize(check);return SQLITE_MISUSE;
  }
  sqlite3_finalize(check);
  char *q=quote_ident(name);if(!q)return SQLITE_NOMEM;
  char *pragma=sqlite3_mprintf("PRAGMA main.table_info(%s)",q);sqlite3_free(q);
  if(!pragma)return SQLITE_NOMEM;
  sqlite3_stmt *st=NULL;rc=prepare(db,&st,pragma);sqlite3_free(pragma);if(rc!=SQLITE_OK)return rc;
  t->name=sqlite3_mprintf("%s",name);if(!t->name){sqlite3_finalize(st);return SQLITE_NOMEM;}
  int *pk_ranks=NULL;
  while((rc=sqlite3_step(st))==SQLITE_ROW){
    const char *col=(const char*)sqlite3_column_text(st,1);
    int n=t->ncols+1;
    char **cols=sqlite3_realloc64(t->columns,(sqlite3_uint64)n*sizeof(char*));
    if(!cols){rc=SQLITE_NOMEM;break;}
    t->columns=cols;t->columns[t->ncols]=NULL;
    int *ranks=sqlite3_realloc64(pk_ranks,(sqlite3_uint64)n*sizeof(int));
    if(!ranks){rc=SQLITE_NOMEM;break;}pk_ranks=ranks;
    t->columns[t->ncols]=sqlite3_mprintf("%s",col?col:"");
    if(!t->columns[t->ncols]){rc=SQLITE_NOMEM;break;}
    pk_ranks[t->ncols]=sqlite3_column_int(st,5);t->ncols=n;
    if(strcmp(col,ROW_CID)==0 || strcmp(col,DELETE_CID)==0){rc=SQLITE_SCHEMA;break;}
  }
  sqlite3_finalize(st);if(rc==SQLITE_DONE)rc=SQLITE_OK;
  if(rc==SQLITE_OK){
    for(int i=0;i<t->ncols;i++)if(pk_ranks[i]>0)t->npk++;
    if(!t->npk)rc=SQLITE_SCHEMA;
    else {t->pk_index=sqlite3_malloc64((sqlite3_uint64)t->npk*sizeof(int));
      if(!t->pk_index)rc=SQLITE_NOMEM;
      else for(int i=0;i<t->npk;i++){
        int found=0;for(int j=0;j<t->ncols;j++)if(pk_ranks[j]==i+1){t->pk_index[i]=j;found=1;break;}
        if(!found){rc=SQLITE_SCHEMA;break;}
      }
    }
  }
  sqlite3_free(pk_ranks);
  if(rc!=SQLITE_OK)free_table(t);
  return rc;
}
static int col_index(const TableInfo *t,const char *col){
  for(int i=0;i<t->ncols;i++)if(strcmp(t->columns[i],col)==0)return i;
  return -1;
}
static int is_pk(const TableInfo *t,int col){
  for(int i=0;i<t->npk;i++)if(t->pk_index[i]==col)return 1;
  return 0;
}
static int bind_key_item(sqlite3_stmt *stmt,int pos,const unsigned char *ptr,int n,int *offset) {
  int k=*offset;if(k>=n)return SQLITE_CORRUPT;
  unsigned char kind=ptr[k++];
  if(kind=='I'||kind=='R'){
    if(k>n-8)return SQLITE_CORRUPT;
    uint64_t u=0;for(int j=0;j<8;j++)u=(u<<8)|ptr[k++];
    if(kind=='I')sqlite3_bind_int64(stmt,pos,(sqlite3_int64)u);
    else {double d;memcpy(&d,&u,8);sqlite3_bind_double(stmt,pos,d);}
  }else if(kind=='T'||kind=='B'){
    if(k>n-4)return SQLITE_CORRUPT;
    uint32_t len=0;for(int j=0;j<4;j++)len=(len<<8)|ptr[k++];
    if(len>(uint32_t)(n-k))return SQLITE_CORRUPT;
    if(kind=='T')sqlite3_bind_text(stmt,pos,(const char*)ptr+k,(int)len,SQLITE_TRANSIENT);
    else sqlite3_bind_blob(stmt,pos,ptr+k,(int)len,SQLITE_TRANSIENT);
    k+=(int)len;
  }else return SQLITE_CORRUPT;
  *offset=k;return SQLITE_OK;
}
static int bind_key(sqlite3_stmt *s,const TableInfo *t,const void *pk,int n){
  if(!pk||n<=0)return SQLITE_CORRUPT;
  const unsigned char *p=(const unsigned char*)pk;int off=0,rc;
  for(int i=0;i<t->npk;i++){
    rc=bind_key_item(s,i+1,p,n,&off);if(rc!=SQLITE_OK)return rc;
  }
  return off==n?SQLITE_OK:SQLITE_CORRUPT;
}
static int apply_data(sqlite3 *db,const TableInfo *t,const void *pk,int n,const char *cid,sqlite3_value *value){
  int del=strcmp(cid,DELETE_CID)==0,row=strcmp(cid,ROW_CID)==0;
  if(!del&&!row){int idx=col_index(t,cid);if(idx<0||is_pk(t,idx))return SQLITE_SCHEMA;}
  char *tn=quote_ident(t->name),*sql=NULL;int rc=SQLITE_OK;
  if(!tn)return SQLITE_NOMEM;
  if(del){
    sql=sqlite3_mprintf("DELETE FROM %s WHERE ",tn);
    for(int i=0;i<t->npk;i++){
      char *q=quote_ident(t->columns[t->pk_index[i]]);
      sql=sqlite3_mprintf("%z%s%s=?%d",sql,i?" AND ":"",q,i+1);sqlite3_free(q);
    }
  }else{
    sql=sqlite3_mprintf("INSERT INTO %s (",tn);
    for(int i=0;i<t->npk;i++){
      char *q=quote_ident(t->columns[t->pk_index[i]]);
      sql=sqlite3_mprintf("%z%s%s",sql,i?",":"",q);sqlite3_free(q);
    }
    char *col=row?NULL:quote_ident(cid);
    if(!row)sql=sqlite3_mprintf("%z,%s",sql,col);
    sql=sqlite3_mprintf("%z) VALUES (",sql);
    for(int i=0;i<t->npk+(row?0:1);i++)sql=sqlite3_mprintf("%z%s?%d",sql,i?",":"",i+1);
    sql=sqlite3_mprintf("%z) ON CONFLICT DO %s",sql,row?"NOTHING":"UPDATE SET ");
    if(!row)sql=sqlite3_mprintf("%z%s=excluded.%s",sql,col,col);
    sqlite3_free(col);
  }
  sqlite3_free(tn);if(!sql)return SQLITE_NOMEM;
  sqlite3_stmt *stmt=NULL;rc=prepare(db,&stmt,sql);sqlite3_free(sql);
  if(rc==SQLITE_OK){rc=bind_key(stmt,t,pk,n);
    if(rc==SQLITE_OK&&!del&&!row){if(value)sqlite3_bind_value(stmt,t->npk+1,value);else sqlite3_bind_null(stmt,t->npk+1);}
    if(rc==SQLITE_OK)rc=exec_stmt(stmt);
  }
  sqlite3_finalize(stmt);return rc;
}
typedef struct BatchWriter {sqlite3_stmt *cell,*log;} BatchWriter;
static int batch_prepare(sqlite3 *db,BatchWriter *w) {
  memset(w,0,sizeof(*w));
  int rc=prepare(db,&w->cell,
    "INSERT INTO _crdt_cells(table_name,pk,cid,hlc,site_id) VALUES(?1,?2,?3,?4,?5) "
    "ON CONFLICT(table_name,pk,cid) DO UPDATE SET hlc=excluded.hlc,site_id=excluded.site_id");
  if(rc==SQLITE_OK)rc=prepare(db,&w->log,
    "INSERT INTO _crdt_log(site_id,seq,table_name,pk,cid,val,hlc) VALUES(?1,?2,?3,?4,?5,?6,?7)");
  return rc;
}
static void batch_finish(BatchWriter *w) {
  sqlite3_finalize(w->cell);sqlite3_finalize(w->log);
  memset(w,0,sizeof(*w));
}
static int emit_local(sqlite3 *db,const char *table,const void *pk,int npk,const char *cid,
                      sqlite3_value *value,CrdtContext *c,const char *site,BatchWriter *w){
  c->last_clock=tick(db,c->last_clock);c->local_seq++;
  sqlite3_stmt *s=w->cell;
  sqlite3_reset(s);sqlite3_clear_bindings(s);
  sqlite3_bind_text(s,1,table,-1,SQLITE_TRANSIENT);
  sqlite3_bind_blob(s,2,pk,npk,SQLITE_TRANSIENT);
  sqlite3_bind_text(s,3,cid,-1,SQLITE_TRANSIENT);
  sqlite3_bind_int64(s,4,c->last_clock);
  sqlite3_bind_text(s,5,site,-1,SQLITE_TRANSIENT);
  int rc=exec_stmt(s);if(rc!=SQLITE_OK)return rc;
  s=w->log;sqlite3_reset(s);sqlite3_clear_bindings(s);
  sqlite3_bind_text(s,1,site,-1,SQLITE_TRANSIENT);sqlite3_bind_int64(s,2,c->local_seq);
  sqlite3_bind_text(s,3,table,-1,SQLITE_TRANSIENT);sqlite3_bind_blob(s,4,pk,npk,SQLITE_TRANSIENT);
  sqlite3_bind_text(s,5,cid,-1,SQLITE_TRANSIENT);
  if(value)sqlite3_bind_value(s,6,value);else sqlite3_bind_null(s,6);
  sqlite3_bind_int64(s,7,c->last_clock);
  return exec_stmt(s);
}
/* The physical row remains alive when a newer column survived an older delete. */
static int has_newer_row_value(sqlite3 *db,const char *table,const void *pk,int npk,
                               sqlite3_int64 hlc,const char *site,int *newer){
  sqlite3_stmt *s=NULL;*newer=0;
  int rc=prepare(db,&s,
    "SELECT 1 FROM _crdt_cells WHERE table_name=?1 AND pk=?2 AND cid<>?3 "
    "AND (hlc>?4 OR (hlc=?4 AND site_id>?5)) LIMIT 1");
  if(rc==SQLITE_OK){
    sqlite3_bind_text(s,1,table,-1,SQLITE_TRANSIENT);
    sqlite3_bind_blob(s,2,pk,npk,SQLITE_TRANSIENT);
    sqlite3_bind_text(s,3,DELETE_CID,-1,SQLITE_STATIC);
    sqlite3_bind_int64(s,4,hlc);sqlite3_bind_text(s,5,site,-1,SQLITE_TRANSIENT);
    rc=sqlite3_step(s);
    if(rc==SQLITE_ROW){*newer=1;rc=SQLITE_OK;}
    else if(rc==SQLITE_DONE)rc=SQLITE_OK;
  }
  sqlite3_finalize(s);return rc;
}
/* When a delete arrives after a newer field, rebuild the surviving fields.
 * This prevents old columns from leaking through an out-of-order tombstone. */
static int delete_preserving_newer(sqlite3 *db,const TableInfo *t,const void *pk,int npk,
                                   sqlite3_int64 hlc,const char *site){
  int rc=apply_data(db,t,pk,npk,DELETE_CID,NULL);
  if(rc!=SQLITE_OK)return rc;
  sqlite3_stmt *s=NULL;
  rc=prepare(db,&s,
    "SELECT l.cid,l.val FROM _crdt_cells AS c JOIN _crdt_log AS l "
    "ON l.table_name=c.table_name AND l.pk=c.pk AND l.cid=c.cid "
    "AND l.hlc=c.hlc AND l.site_id=c.site_id "
    "WHERE c.table_name=?1 AND c.pk=?2 AND c.cid<>?3 "
    "AND (c.hlc>?4 OR (c.hlc=?4 AND c.site_id>?5)) "
    "ORDER BY (c.cid<>?6), c.cid");
  if(rc==SQLITE_OK){
    sqlite3_bind_text(s,1,t->name,-1,SQLITE_TRANSIENT);
    sqlite3_bind_blob(s,2,pk,npk,SQLITE_TRANSIENT);
    sqlite3_bind_text(s,3,DELETE_CID,-1,SQLITE_STATIC);
    sqlite3_bind_int64(s,4,hlc);
    sqlite3_bind_text(s,5,site,-1,SQLITE_TRANSIENT);
    sqlite3_bind_text(s,6,ROW_CID,-1,SQLITE_STATIC);
    int step;
    while((step=sqlite3_step(s))==SQLITE_ROW){
      const char *cid=(const char*)sqlite3_column_text(s,0);
      /* sqlite3_column_value is a protected sqlite3_value: bind_value can copy it. */
      sqlite3_value *val=sqlite3_column_value(s,1);
      rc=apply_data(db,t,pk,npk,cid,val);
      if(rc!=SQLITE_OK)break;
    }
    if(rc==SQLITE_OK && step!=SQLITE_DONE)rc=step;
  }
  sqlite3_finalize(s);return rc;
}
static void fn_init(sqlite3_context *ctx,int argc,sqlite3_value **argv){
  (void)argc;
  CrdtContext *c=(CrdtContext*)sqlite3_user_data(ctx);
  sqlite3 *db=sqlite3_context_db_handle(ctx);
  const char *site=(const char*)sqlite3_value_text(argv[0]);
  if(!site||!*site||strlen(site)>128){
    sqlite3_result_error(ctx,"site_id must be a nonempty string of at most 128 bytes",-1);return;
  }
  if(c->session){sqlite3_result_error(ctx,"cannot initialize during an active session",-1);return;}
  /* Metadata and user tables always share main's transaction and WAL. */
  int rc=SQLITE_OK;
  char *ddl=sqlite3_mprintf(
    "SAVEPOINT crdt_init_sp;"
    "CREATE TABLE IF NOT EXISTS main._crdt_config(key TEXT PRIMARY KEY,value NOT NULL) WITHOUT ROWID;"
    "CREATE TABLE IF NOT EXISTS main._crdt_cells("
      "table_name TEXT NOT NULL, pk BLOB NOT NULL,cid TEXT NOT NULL,hlc INTEGER NOT NULL,"
      "site_id TEXT NOT NULL,PRIMARY KEY(table_name,pk,cid)) WITHOUT ROWID;"
    "CREATE TABLE IF NOT EXISTS main._crdt_log("
      "db_version INTEGER PRIMARY KEY AUTOINCREMENT,site_id TEXT NOT NULL,seq INTEGER NOT NULL,"
      "table_name TEXT NOT NULL,pk BLOB NOT NULL,cid TEXT NOT NULL,val BLOB,hlc INTEGER NOT NULL,"
      "UNIQUE(site_id,seq));"
    "CREATE INDEX IF NOT EXISTS main._crdt_log_cell ON _crdt_log(table_name,pk,cid);"
    "CREATE VIEW IF NOT EXISTS main.crdt_changes AS "
      "SELECT db_version,site_id,seq,table_name,pk,cid,val,hlc,"
      "(cid='__crdt_delete') AS deleted FROM _crdt_log;"
    "CREATE VIEW IF NOT EXISTS main.crsql_changes AS "
      "SELECT table_name AS \"table\", pk,cid,val,hlc AS col_version,"
      "db_version,site_id,0 AS cl,seq FROM _crdt_log;"
  );
  if(!ddl){err_sql(ctx,SQLITE_NOMEM);return;}
  rc=run_sql(db,ddl);sqlite3_free(ddl);
  if(rc==SQLITE_OK){
    char *old=NULL;rc=get_node(db,&old);
    if(rc==SQLITE_NOTFOUND){
      sqlite3_stmt *s=NULL;rc=prepare(db,&s,"INSERT INTO _crdt_config(key,value) VALUES('site_id',?1)");
      if(rc==SQLITE_OK){sqlite3_bind_text(s,1,site,-1,SQLITE_TRANSIENT);rc=exec_stmt(s);}sqlite3_finalize(s);
    }else if(rc==SQLITE_OK&&strcmp(old,site)!=0){
      rc=SQLITE_CONSTRAINT;sqlite3_result_error(ctx,"database already initialized for a different site_id",-1);
    }
    sqlite3_free(old);
  }
  if(rc==SQLITE_OK)rc=run_sql(db,"RELEASE crdt_init_sp");
  if(rc!=SQLITE_OK){run_sql(db,"ROLLBACK TO crdt_init_sp; RELEASE crdt_init_sp");
    if(rc!=SQLITE_CONSTRAINT)err_sql(ctx,rc);
    return;
  }
  c->initialized=1;
  sqlite3_result_int(ctx,1);
}
static void fn_storage(sqlite3_context *ctx,int argc,sqlite3_value **argv){
  (void)argc;(void)argv;
  CrdtContext *c=(CrdtContext*)sqlite3_user_data(ctx);
  if(!c->initialized)sqlite3_result_null(ctx);
  else sqlite3_result_text(ctx,"main",-1,SQLITE_STATIC);
}
static void fn_start(sqlite3_context *ctx,int argc,sqlite3_value **argv){
  (void)argc;CrdtContext *c=(CrdtContext*)sqlite3_user_data(ctx);sqlite3 *db=c->db;
  if(c->session){sqlite3_result_error(ctx,"capture is already active",-1);return;}
  if(sqlite3_get_autocommit(db)){sqlite3_result_error(ctx,"start capture inside BEGIN transaction",-1);return;}
  if(require_initialized(ctx)!=SQLITE_OK)return;
  const char *table=(const char*)sqlite3_value_text(argv[0]);TableInfo t;
  int rc=load_table(db,table,&t);if(rc!=SQLITE_OK){err_sql(ctx,rc);return;}
  free_table(&t);
  rc=sqlite3session_create(db,"main",&c->session);
  if(rc==SQLITE_OK)rc=sqlite3session_attach(c->session,table);
  if(rc==SQLITE_OK)rc=get_config_i64(db,"clock",&c->last_clock);
  if(rc==SQLITE_OK)rc=get_config_i64(db,"local_seq",&c->local_seq);
  if(rc!=SQLITE_OK){if(c->session)sqlite3session_delete(c->session);c->session=NULL;err_sql(ctx,rc);return;}
  sqlite3_result_int(ctx,1);
}
static void fn_track(sqlite3_context *ctx,int argc,sqlite3_value **argv){
  (void)argc;CrdtContext *c=(CrdtContext*)sqlite3_user_data(ctx);
  if(!c->session){sqlite3_result_error(ctx,"call crdt_start(table) first",-1);return;}
  const char *table=(const char*)sqlite3_value_text(argv[0]);TableInfo t;
  int rc=load_table(c->db,table,&t);if(rc!=SQLITE_OK){err_sql(ctx,rc);return;}
  free_table(&t);rc=sqlite3session_attach(c->session,table);
  if(rc!=SQLITE_OK){err_sql(ctx,rc);return;}sqlite3_result_int(ctx,1);
}
static void fn_abort(sqlite3_context *ctx,int argc,sqlite3_value **argv){
  (void)argc;(void)argv;CrdtContext *c=(CrdtContext*)sqlite3_user_data(ctx);
  if(c->session){sqlite3session_delete(c->session);c->session=NULL;}
  sqlite3_result_int(ctx,1);
}
static void fn_flush(sqlite3_context *ctx,int argc,sqlite3_value **argv){
  (void)argc;(void)argv;CrdtContext *c=(CrdtContext*)sqlite3_user_data(ctx);
  sqlite3 *db=c->db;
  if(!c->session){sqlite3_result_error(ctx,"no active capture",-1);return;}
  if(sqlite3_get_autocommit(db)){sqlite3_result_error(ctx,"flush before COMMIT, inside transaction",-1);return;}
  char *site=NULL;int rc=get_node(db,&site);if(rc!=SQLITE_OK){err_sql(ctx,rc);return;}
  int nbytes=0;void *bytes=NULL;rc=sqlite3session_changeset(c->session,&nbytes,&bytes);
  sqlite3_changeset_iter *it=NULL;
  if(rc==SQLITE_OK)rc=sqlite3changeset_start(&it,nbytes,bytes);
  if(rc==SQLITE_OK)rc=run_sql(db,"SAVEPOINT crdt_flush_sp");
  int emitted=0;TableInfo t={0};const char *loaded=NULL;
  BatchWriter writer={0};if(rc==SQLITE_OK)rc=batch_prepare(db,&writer);
  while(rc==SQLITE_OK){
    int step=sqlite3changeset_next(it);if(step==SQLITE_DONE)break;
    if(step!=SQLITE_ROW){rc=step;break;}
    const char *table=NULL;int ncols=0,op=0;int indirect=0;
    rc=sqlite3changeset_op(it,&table,&ncols,&op,&indirect);if(rc!=SQLITE_OK)break;
    if(indirect)continue;
    if(!loaded || strcmp(table,loaded)!=0){
      free_table(&t);rc=load_table(db,table,&t);if(rc!=SQLITE_OK)break;loaded=t.name;
    }
    if(ncols!=t.ncols){rc=SQLITE_SCHEMA;break;}
    unsigned char *pk_mask=NULL;rc=sqlite3changeset_pk(it,&pk_mask,&ncols);if(rc!=SQLITE_OK)break;
    KeyBuffer key={0};rc=make_key(it,op,&t,pk_mask,&key);
    if(rc==SQLITE_OK){
      if(op==SQLITE_DELETE){
        rc=emit_local(db,table,key.ptr,key.len,DELETE_CID,NULL,c,site,&writer);
        if(rc==SQLITE_OK)emitted++;
      }else{
        /* Non-PK fields already materialize the row. Only PK-only tables
         * need an explicit row marker; this avoids one metadata write per insert. */
        if(op==SQLITE_INSERT && t.ncols==t.npk){
          rc=emit_local(db,table,key.ptr,key.len,ROW_CID,NULL,c,site,&writer);
          if(rc==SQLITE_OK)emitted++;
        }
        for(int j=0;j<t.ncols && rc==SQLITE_OK;j++){
          if(is_pk(&t,j))continue;
          sqlite3_value *v=NULL;
          rc=sqlite3changeset_new(it,j,&v);if(rc!=SQLITE_OK)break;
          if(op==SQLITE_UPDATE && !v)continue;
          rc=emit_local(db,table,key.ptr,key.len,t.columns[j],v,c,site,&writer);
          if(rc==SQLITE_OK)emitted++;
        }
      }
    }
    sqlite3_free(key.ptr);
  }
  free_table(&t);batch_finish(&writer);
  if(rc==SQLITE_OK)rc=set_config_i64(db,"clock",c->last_clock);
  if(rc==SQLITE_OK)rc=set_config_i64(db,"local_seq",c->local_seq);
  if(rc==SQLITE_OK)rc=run_sql(db,"RELEASE crdt_flush_sp");
  else run_sql(db,"ROLLBACK TO crdt_flush_sp; RELEASE crdt_flush_sp");
  if(it)sqlite3changeset_finalize(it);
  sqlite3_free(bytes);sqlite3_free(site);
  /* Always detach: a failed flush cannot be safely retried after statement rollback. */
  sqlite3session_delete(c->session);c->session=NULL;
  if(rc!=SQLITE_OK){err_sql(ctx,rc);return;}
  sqlite3_result_int(ctx,emitted);
}
/* Export an initial snapshot of a pre-existing table as locally-originated
 * CRDT events.  It is intentionally restricted to tables without existing
 * CRDT metadata, so it cannot overwrite genuine replicated versions. */
static void fn_seed(sqlite3_context *ctx,int argc,sqlite3_value **argv){
  (void)argc;
  CrdtContext *c=(CrdtContext*)sqlite3_user_data(ctx);
  sqlite3 *db=c->db;
  if(c->session){sqlite3_result_error(ctx,"cannot seed during capture",-1);return;}
  if(sqlite3_get_autocommit(db)){
    sqlite3_result_error(ctx,"crdt_seed requires a BEGIN transaction",-1);return;
  }
  if(require_initialized(ctx)!=SQLITE_OK)return;
  const char *name=(const char*)sqlite3_value_text(argv[0]);
  TableInfo t={0};int rc=load_table(db,name,&t);
  if(rc!=SQLITE_OK){err_sql(ctx,rc);return;}
  sqlite3_stmt *test=NULL,*rows=NULL;
  char *site=NULL;BatchWriter w={0};
  rc=prepare(db,&test,"SELECT 1 FROM _crdt_cells WHERE table_name=?1 LIMIT 1");
  if(rc==SQLITE_OK){
    sqlite3_bind_text(test,1,name,-1,SQLITE_TRANSIENT);
    int step=sqlite3_step(test);
    if(step==SQLITE_ROW){
      sqlite3_result_error(ctx,"table already has CRDT state; snapshot refused",-1);
      rc=SQLITE_ABORT;
    }else if(step!=SQLITE_DONE)rc=step;
  }
  sqlite3_finalize(test);
  if(rc!=SQLITE_OK){free_table(&t);if(rc!=SQLITE_ABORT)err_sql(ctx,rc);return;}
  rc=get_node(db,&site);
  if(rc==SQLITE_OK)rc=get_config_i64(db,"clock",&c->last_clock);
  if(rc==SQLITE_OK)rc=get_config_i64(db,"local_seq",&c->local_seq);
  if(rc==SQLITE_OK)rc=run_sql(db,"SAVEPOINT crdt_seed_sp");
  if(rc==SQLITE_OK)rc=batch_prepare(db,&w);
  char *qname=quote_ident(name);
  char *sql=qname?sqlite3_mprintf("SELECT * FROM main.%s",qname):NULL;
  sqlite3_free(qname);
  if(rc==SQLITE_OK)rc=sql?prepare(db,&rows,sql):SQLITE_NOMEM;
  sqlite3_free(sql);
  int emitted=0;
  if(rc==SQLITE_OK && sqlite3_column_count(rows)!=t.ncols)rc=SQLITE_SCHEMA;
  while(rc==SQLITE_OK){
    int step=sqlite3_step(rows);
    if(step==SQLITE_DONE)break;
    if(step!=SQLITE_ROW){rc=step;break;}
    KeyBuffer key={0};
    for(int j=0;j<t.npk && rc==SQLITE_OK;j++){
      sqlite3_value *v=sqlite3_column_value(rows,t.pk_index[j]);
      rc=kb_value(&key,v);
    }
    if(rc==SQLITE_OK && t.ncols==t.npk){
      rc=emit_local(db,name,key.ptr,key.len,ROW_CID,NULL,c,site,&w);
      if(rc==SQLITE_OK)emitted++;
    }
    for(int j=0;j<t.ncols && rc==SQLITE_OK;j++){
      if(is_pk(&t,j))continue;
      sqlite3_value *v=sqlite3_column_value(rows,j);
      rc=emit_local(db,name,key.ptr,key.len,t.columns[j],v,c,site,&w);
      if(rc==SQLITE_OK)emitted++;
    }
    sqlite3_free(key.ptr);
  }
  sqlite3_finalize(rows);batch_finish(&w);
  if(rc==SQLITE_OK)rc=set_config_i64(db,"clock",c->last_clock);
  if(rc==SQLITE_OK)rc=set_config_i64(db,"local_seq",c->local_seq);
  if(rc==SQLITE_OK)rc=run_sql(db,"RELEASE crdt_seed_sp");
  else run_sql(db,"ROLLBACK TO crdt_seed_sp; RELEASE crdt_seed_sp");
  free_table(&t);sqlite3_free(site);
  if(rc!=SQLITE_OK){err_sql(ctx,rc);return;}
  sqlite3_result_int(ctx,emitted);
}

static void fn_apply(sqlite3_context *ctx,int argc,sqlite3_value **argv){
  (void)argc;CrdtContext *c=(CrdtContext*)sqlite3_user_data(ctx);sqlite3 *db=c->db;
  if(c->session){sqlite3_result_error(ctx,"cannot apply remote changes during local capture",-1);return;}
  if(require_initialized(ctx)!=SQLITE_OK)return;
  const char *table=(const char*)sqlite3_value_text(argv[0]);
  const void *pk=sqlite3_value_blob(argv[1]);int npk=sqlite3_value_bytes(argv[1]);
  const char *cid=(const char*)sqlite3_value_text(argv[2]);
  sqlite3_value *value=argv[3];sqlite3_int64 hlc=sqlite3_value_int64(argv[4]);
  const char *site=(const char*)sqlite3_value_text(argv[5]);sqlite3_int64 seq=sqlite3_value_int64(argv[6]);
  if(!table||!cid||!site||!*site||npk<=0||!pk||hlc<=0||seq<=0||strlen(site)>128){
    sqlite3_result_error(ctx,"invalid change fields",-1);return;
  }
  TableInfo t;int rc=load_table(db,table,&t);if(rc!=SQLITE_OK){err_sql(ctx,rc);return;}
  if(strcmp(cid,DELETE_CID)!=0 && strcmp(cid,ROW_CID)!=0){
    int col=col_index(&t,cid);if(col<0||is_pk(&t,col)){free_table(&t);sqlite3_result_error(ctx,"invalid CRDT column",-1);return;}
  }
  rc=run_sql(db,"SAVEPOINT crdt_apply_sp");
  sqlite3_int64 old=0,tomb=0,marker=0;char *oldsite=NULL,*tombsite=NULL,*markersite=NULL;
  int accepted=0;
  if(rc==SQLITE_OK)rc=read_version(db,table,pk,npk,cid,&old,&oldsite);
  if(rc==SQLITE_OK)rc=read_version(db,table,pk,npk,DELETE_CID,&tomb,&tombsite);
  if(rc==SQLITE_OK && strcmp(cid,DELETE_CID)!=0)
    rc=read_version(db,table,pk,npk,ROW_CID,&marker,&markersite);
  if(rc==SQLITE_OK){
    int wins=!oldsite || version_wins(hlc,site,old,oldsite);
    if(strcmp(cid,DELETE_CID)!=0 && tombsite && !version_wins(hlc,site,tomb,tombsite))wins=0;
    if(strcmp(cid,DELETE_CID)==0 && markersite && !version_wins(hlc,site,marker,markersite))wins=0;
    if(wins){
      /* A new insertion marker must not erase previously-applied newer cells. */
      int newer=0;
      if(strcmp(cid,DELETE_CID)==0)
        rc=has_newer_row_value(db,table,pk,npk,hlc,site,&newer);
      if(rc==SQLITE_OK){
        if(newer)rc=delete_preserving_newer(db,&t,pk,npk,hlc,site);
        else rc=apply_data(db,&t,pk,npk,cid,value);
      }
      if(rc==SQLITE_OK)rc=write_cell(db,table,pk,npk,cid,hlc,site);
      if(rc==SQLITE_OK)rc=append_log(db,table,pk,npk,cid,value,hlc,site,seq);
      if(rc==SQLITE_OK){accepted=1;
        sqlite3_int64 saved=0;rc=get_config_i64(db,"clock",&saved);
        if(rc==SQLITE_OK && hlc>saved)rc=set_config_i64(db,"clock",hlc);
      }
    }
  }
  sqlite3_free(oldsite);sqlite3_free(tombsite);sqlite3_free(markersite);free_table(&t);
  if(rc==SQLITE_CONSTRAINT_UNIQUE || rc==SQLITE_CONSTRAINT_PRIMARYKEY){
    /* Replayed source events are idempotent. */
    rc=SQLITE_OK;accepted=0;run_sql(db,"ROLLBACK TO crdt_apply_sp");
  }
  if(rc==SQLITE_OK)rc=run_sql(db,"RELEASE crdt_apply_sp");
  else run_sql(db,"ROLLBACK TO crdt_apply_sp; RELEASE crdt_apply_sp");
  if(rc!=SQLITE_OK){err_sql(ctx,rc);return;}sqlite3_result_int(ctx,accepted);
}
static void fn_clock(sqlite3_context *ctx,int argc,sqlite3_value **argv){
  (void)argc;(void)argv;sqlite3_int64 v=0;int rc=get_config_i64(sqlite3_context_db_handle(ctx),"clock",&v);
  if(rc!=SQLITE_OK){err_sql(ctx,rc);return;}sqlite3_result_int64(ctx,v);
}
static void fn_site(sqlite3_context *ctx,int argc,sqlite3_value **argv){
  (void)argc;(void)argv;char *s=NULL;int rc=get_node(sqlite3_context_db_handle(ctx),&s);
  if(rc!=SQLITE_OK){sqlite3_result_null(ctx);return;}sqlite3_result_text(ctx,s,-1,sqlite3_free);
}
#ifdef _WIN32
__declspec(dllexport)
#endif
int sqlite3_crdtsession_init(sqlite3 *db,char **pzErrMsg,const sqlite3_api_routines *pApi){
  (void)pzErrMsg;SQLITE_EXTENSION_INIT2(pApi);
  CrdtContext *c=sqlite3_malloc64(sizeof(CrdtContext));if(!c)return SQLITE_NOMEM;
  memset(c,0,sizeof(*c));c->db=db;
  /* The first function owns the shared context destructor. Other functions are
     scoped to this connection. SQLite drops functions when connection closes. */
  int flags=SQLITE_UTF8|SQLITE_DIRECTONLY;
  int rc=sqlite3_create_function_v2(db,"crdt_init",1,flags,c,fn_init,NULL,NULL,destroy_context);
  if(rc!=SQLITE_OK)return rc;
  struct Reg {const char *name;int arity;void (*fn)(sqlite3_context*,int,sqlite3_value**);} regs[]={
    {"crdt_storage",0,fn_storage},{"crdt_start",1,fn_start},{"crdt_track",1,fn_track},{"crdt_abort",0,fn_abort},
    {"crdt_flush",0,fn_flush},{"crdt_seed",1,fn_seed},{"crdt_apply",7,fn_apply},{"crdt_clock",0,fn_clock},
    {"crdt_site_id",0,fn_site}
  };
  for(unsigned i=0;i<sizeof(regs)/sizeof(regs[0]);i++){
    rc=sqlite3_create_function_v2(db,regs[i].name,regs[i].arity,flags,c,regs[i].fn,NULL,NULL,NULL);
    if(rc!=SQLITE_OK)return rc;
  }
  return SQLITE_OK;
}
#ifdef _WIN32
__declspec(dllexport)
#endif
int sqlite3_extension_init(sqlite3 *db,char **pzErrMsg,const sqlite3_api_routines *pApi){
  return sqlite3_crdtsession_init(db,pzErrMsg,pApi);
}