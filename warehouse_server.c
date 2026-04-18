/*
 * ============================================================
 *   WAREHOUSE INVENTORY MANAGEMENT SYSTEM — Web Server
 *   Pure C HTTP server (POSIX sockets) on port 8080
 *   Serves index.html + JSON REST API
 *
 *   Build:  gcc -Wall -Wextra -o warehouse_server warehouse_server.c
 *   Run:    ./warehouse_server
 *   Open:   http://localhost:8080
 * ============================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>

/* ============================================================
 *  CONSTANTS
 * ============================================================ */
#define PORT        8080
#define TABLE_SIZE  53
#define NAME_LEN    64
#define BUF_SIZE    16384
#define RESP_SIZE   65536

#define ACTION_ADD  'A'
#define ACTION_DEL  'D'
#define ACTION_UPD  'U'
#define ACTION_ORD  'O'
#define ACTION_DIS  'S'

/* ============================================================
 *  DATA STRUCTURES
 * ============================================================ */
typedef struct Item {
    int          id;
    char         name[NAME_LEN];
    int          quantity;
    int          min_threshold;
    struct Item *next;
} Item;

typedef struct Order {
    int          id;
    int          quantity;
    struct Order*next;
} Order;

typedef struct Action {
    char          action_type;
    int           id;
    int           quantity;
    char          name[NAME_LEN];
    int           min_threshold;
    int           old_quantity;
    struct Action*next;
} Action;

typedef struct { Item  *buckets[TABLE_SIZE]; } HashTable;
typedef struct { Order *front; Order *rear; int size; } Queue;
typedef struct { Action *top;  int size; } Stack;

typedef struct {
    int  ok;
    char msg[512];
} Result;

/* ============================================================
 *  GLOBAL STATE
 * ============================================================ */
static HashTable g_table;
static Queue     g_orders;
static Stack     g_undo;

/* ============================================================
 *  HASH TABLE
 * ============================================================ */
static int hash(int id) {
    int h = id % TABLE_SIZE;
    if (h < 0) h += TABLE_SIZE;
    return h;
}
static Item *ht_search(int id) {
    Item *n = g_table.buckets[hash(id)];
    while (n) { if (n->id == id) return n; n = n->next; }
    return NULL;
}
static void ht_insert(Item *item) {
    int idx = hash(item->id);
    item->next = g_table.buckets[idx];
    g_table.buckets[idx] = item;
}
static int ht_remove(int id) {
    int idx = hash(id); Item *prev = NULL, *cur = g_table.buckets[idx];
    while (cur) {
        if (cur->id == id) {
            if (prev) prev->next = cur->next;
            else      g_table.buckets[idx] = cur->next;
            free(cur); return 1;
        }
        prev = cur; cur = cur->next;
    }
    return 0;
}

/* ============================================================
 *  STACK
 * ============================================================ */
static void stack_push(char type, int id, int qty,
                       const char *name, int thr, int old_qty) {
    Action *a = malloc(sizeof(Action));
    if (!a) return;
    a->action_type = type; a->id = id; a->quantity = qty;
    a->min_threshold = thr; a->old_quantity = old_qty;
    strncpy(a->name, name ? name : "", NAME_LEN - 1);
    a->name[NAME_LEN-1] = '\0';
    a->next = g_undo.top; g_undo.top = a; g_undo.size++;
}
static Action *stack_pop(void) {
    if (!g_undo.top) return NULL;
    Action *a = g_undo.top; g_undo.top = a->next; g_undo.size--;
    a->next = NULL; return a;
}

/* ============================================================
 *  QUEUE
 * ============================================================ */
static void queue_enqueue(int id, int qty) {
    Order *o = malloc(sizeof(Order));
    if (!o) return;
    o->id = id; o->quantity = qty; o->next = NULL;
    if (g_orders.rear) g_orders.rear->next = o;
    else               g_orders.front = o;
    g_orders.rear = o; g_orders.size++;
}
static Order *queue_dequeue(void) {
    if (!g_orders.front) return NULL;
    Order *o = g_orders.front; g_orders.front = o->next;
    if (!g_orders.front) g_orders.rear = NULL;
    g_orders.size--; o->next = NULL; return o;
}
static void queue_enqueue_front(int id, int qty) {
    Order *o = malloc(sizeof(Order));
    if (!o) return;
    o->id = id; o->quantity = qty; o->next = g_orders.front;
    g_orders.front = o;
    if (!g_orders.rear) g_orders.rear = o;
    g_orders.size++;
}

/* ============================================================
 *  BUSINESS OPERATIONS  (return Result instead of printf)
 * ============================================================ */
#define RES_ERR(fmt,...) do { r.ok=0; snprintf(r.msg,sizeof(r.msg),fmt,##__VA_ARGS__); return r; } while(0)
#define RES_OK(fmt,...)  do { r.ok=1; snprintf(r.msg,sizeof(r.msg),fmt,##__VA_ARGS__); return r; } while(0)

static Result op_add(int id, const char *name, int qty, int thr) {
    Result r;
    if (id <= 0)             RES_ERR("ID must be positive.");
    if (ht_search(id))       RES_ERR("Item ID %d already exists.", id);
    if (qty < 0)             RES_ERR("Quantity cannot be negative.");
    if (thr < 0)             RES_ERR("Threshold cannot be negative.");
    if (!name||!*name)       RES_ERR("Name cannot be empty.");
    Item *item = malloc(sizeof(Item));
    if (!item)               RES_ERR("Memory allocation failed.");
    item->id=id; item->quantity=qty; item->min_threshold=thr; item->next=NULL;
    strncpy(item->name, name, NAME_LEN-1); item->name[NAME_LEN-1]='\0';
    ht_insert(item);
    stack_push(ACTION_ADD, id, qty, name, thr, 0);
    if (qty <= thr)
        RES_OK("Item '%s' (ID %d) added. ⚠ Stock at/below threshold!", name, id);
    RES_OK("Item '%s' (ID %d) added successfully.", name, id);
}

static Result op_delete(int id) {
    Result r;
    Item *item = ht_search(id);
    if (!item) RES_ERR("Item ID %d not found.", id);
    stack_push(ACTION_DEL, id, item->quantity, item->name, item->min_threshold, 0);
    ht_remove(id);
    RES_OK("Item ID %d deleted successfully.", id);
}

static Result op_search(int id) {
    Result r;
    Item *item = ht_search(id);
    if (!item) RES_ERR("Item ID %d not found.", id);
    snprintf(r.msg, sizeof(r.msg),
             "{\"id\":%d,\"name\":\"%s\",\"quantity\":%d,\"threshold\":%d,\"status\":\"%s\"}",
             item->id, item->name, item->quantity, item->min_threshold,
             item->quantity <= item->min_threshold ? "LOW" : "OK");
    r.ok = 1; return r;
}

static Result op_update(int id, int delta) {
    Result r;
    Item *item = ht_search(id);
    if (!item) RES_ERR("Item ID %d not found.", id);
    int nq = item->quantity + delta;
    if (nq < 0) RES_ERR("Would result in negative stock (%d). Aborted.", nq);
    stack_push(ACTION_UPD, id, delta, item->name, item->min_threshold, item->quantity);
    item->quantity = nq;
    if (nq <= item->min_threshold)
        RES_OK("Stock updated: %d → %d. ⚠ Low stock warning!", nq-delta, nq);
    RES_OK("Stock updated: %d → %d.", nq-delta, nq);
}

static Result op_add_order(int id, int qty) {
    Result r;
    if (id <= 0)  RES_ERR("ID must be positive.");
    if (qty <= 0) RES_ERR("Quantity must be positive.");
    Item *item = ht_search(id);
    if (!item)    RES_ERR("Item ID %d not found.", id);
    if (item->quantity < qty)
        RES_ERR("Insufficient stock. Available: %d, Requested: %d.", item->quantity, qty);
    queue_enqueue(id, qty);
    stack_push(ACTION_ORD, id, qty, item->name, item->min_threshold, 0);
    RES_OK("Order placed — Item '%s' (ID %d), Qty %d. Queue size: %d.",
           item->name, id, qty, g_orders.size);
}

static Result op_dispatch(void) {
    Result r;
    if (!g_orders.front) RES_ERR("Order queue is empty.");
    Order *o = queue_dequeue();
    Item  *item = ht_search(o->id);
    if (!item) { free(o); RES_ERR("Item ID %d no longer exists. Order cancelled.", o->id); }
    if (item->quantity < o->quantity) {
        int need = o->quantity; free(o);
        RES_ERR("Insufficient stock to dispatch (need %d, have %d).", need, item->quantity);
    }
    int dqty = o->quantity;
    item->quantity -= dqty;
    stack_push(ACTION_DIS, item->id, dqty, item->name, item->min_threshold, 0);
    free(o);
    if (item->quantity <= item->min_threshold)
        RES_OK("Dispatched '%s' x%d. Remaining: %d. ⚠ Low stock!", item->name, dqty, item->quantity);
    RES_OK("Dispatched '%s' x%d. Remaining stock: %d.", item->name, dqty, item->quantity);
}

static Result op_undo(void) {
    Result r;
    Action *a = stack_pop();
    if (!a) RES_ERR("Nothing to undo (stack is empty).");

    switch (a->action_type) {
    case ACTION_ADD:
        if (ht_remove(a->id)) { r.ok=1; snprintf(r.msg,sizeof(r.msg),"Undo ADD: Item ID %d removed.",a->id); }
        else                  { r.ok=0; snprintf(r.msg,sizeof(r.msg),"Undo ADD: Item ID %d not found.",a->id); }
        break;
    case ACTION_DEL: {
        if (ht_search(a->id)) { r.ok=0; snprintf(r.msg,sizeof(r.msg),"Undo DEL: ID %d already exists.",a->id); break; }
        Item *item = malloc(sizeof(Item));
        if (!item) { r.ok=0; snprintf(r.msg,sizeof(r.msg),"Memory error."); break; }
        item->id=a->id; item->quantity=a->quantity; item->min_threshold=a->min_threshold; item->next=NULL;
        strncpy(item->name,a->name,NAME_LEN-1); item->name[NAME_LEN-1]='\0';
        ht_insert(item);
        r.ok=1; snprintf(r.msg,sizeof(r.msg),"Undo DEL: Item '%s' (ID %d) restored.",a->name,a->id);
        break; }
    case ACTION_UPD: {
        Item *item = ht_search(a->id);
        if (!item) { r.ok=0; snprintf(r.msg,sizeof(r.msg),"Undo UPD: ID %d not found.",a->id); break; }
        int prev = item->quantity;
        item->quantity = a->old_quantity;
        r.ok=1; snprintf(r.msg,sizeof(r.msg),"Undo UPDATE: '%s' stock restored %d → %d.",item->name,prev,a->old_quantity);
        break; }
    case ACTION_ORD: {
        Order *cur = g_orders.rear;
        if (!cur) { r.ok=0; snprintf(r.msg,sizeof(r.msg),"Undo ORDER: Queue is empty."); break; }
        if (cur->id==a->id && cur->quantity==a->quantity) {
            if (g_orders.front==g_orders.rear) { g_orders.front=g_orders.rear=NULL; }
            else { Order *prev=g_orders.front; while(prev->next!=cur) prev=prev->next; prev->next=NULL; g_orders.rear=prev; }
            g_orders.size--; free(cur);
            r.ok=1; snprintf(r.msg,sizeof(r.msg),"Undo ORDER: Order for ID %d (x%d) removed.",a->id,a->quantity);
        } else { r.ok=0; snprintf(r.msg,sizeof(r.msg),"Undo ORDER: Matching order not at queue rear."); }
        break; }
    case ACTION_DIS: {
        Item *item = ht_search(a->id);
        if (!item) { r.ok=0; snprintf(r.msg,sizeof(r.msg),"Undo DISPATCH: ID %d not found.",a->id); break; }
        item->quantity += a->quantity;
        queue_enqueue_front(a->id, a->quantity);
        r.ok=1; snprintf(r.msg,sizeof(r.msg),"Undo DISPATCH: '%s' stock +%d. Order re-queued.",item->name,a->quantity);
        break; }
    default:
        r.ok=0; snprintf(r.msg,sizeof(r.msg),"Unknown action type '%c'.",a->action_type);
    }
    free(a); return r;
}

static Result op_export(void) {
    Result r;
    FILE *fp = fopen("inventory.csv","w");
    if (!fp) RES_ERR("Could not open inventory.csv for writing.");
    fprintf(fp,"ID,Name,Quantity,MinThreshold,Status\n");
    int count=0;
    for (int i=0;i<TABLE_SIZE;i++) {
        Item *n=g_table.buckets[i];
        while(n) {
            fprintf(fp,"%d,\"%s\",%d,%d,%s\n",n->id,n->name,n->quantity,n->min_threshold,
                    n->quantity<=n->min_threshold?"LOW":"OK");
            count++; n=n->next;
        }
    }
    fclose(fp);
    RES_OK("Exported %d item(s) to inventory.csv.", count);
}

/* ============================================================
 *  CLEANUP
 * ============================================================ */
static void cleanup(void) {
    for (int i=0;i<TABLE_SIZE;i++) { Item *n=g_table.buckets[i]; while(n){Item *t=n->next;free(n);n=t;} g_table.buckets[i]=NULL; }
    Order *o=g_orders.front; while(o){Order *t=o->next;free(o);o=t;} g_orders.front=g_orders.rear=NULL;
    Action *a=g_undo.top; while(a){Action *t=a->next;free(a);a=t;} g_undo.top=NULL;
}

/* ============================================================
 *  JSON HELPERS
 * ============================================================ */
static int json_int(const char *json, const char *key, int *out) {
    char srch[NAME_LEN+4]; snprintf(srch,sizeof(srch),"\"%s\":",key);
    const char *p=strstr(json,srch); if(!p) return 0;
    p+=strlen(srch); while(*p==' ')p++;
    if(!isdigit((unsigned char)*p)&&*p!='-') return 0;
    *out=atoi(p); return 1;
}
static int json_str(const char *json, const char *key, char *out, int len) {
    char srch[NAME_LEN+4]; snprintf(srch,sizeof(srch),"\"%s\":",key);
    const char *p=strstr(json,srch); if(!p) return 0;
    p+=strlen(srch); while(*p==' ')p++;
    if(*p!='"') return 0; p++;
    int i=0;
    while(*p && *p!='"' && i<len-1){ if(*p=='\\'&&*(p+1))p++; out[i++]=*p++; }
    out[i]='\0'; return 1;
}

/* ============================================================
 *  HTTP HELPERS
 * ============================================================ */
static void http_send(int fd, int status, const char *ctype,
                      const char *body, size_t blen) {
    char hdr[512];
    const char *st = status==200?"200 OK":status==400?"400 Bad Request":"404 Not Found";
    int hlen = snprintf(hdr,sizeof(hdr),
        "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
        "Access-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n",
        st, ctype, blen);
    write(fd,hdr,hlen);
    write(fd,body,blen);
}
static void send_json(int fd, int status, const char *json) {
    http_send(fd,status,"application/json; charset=utf-8",json,strlen(json));
}
static void result_json(int fd, Result r) {
    char buf[600];
    /* Escape backslash and double-quote in msg for safe JSON */
    char esc[512]; int ei=0;
    for(int i=0;r.msg[i]&&ei<(int)sizeof(esc)-3;i++){
        if(r.msg[i]=='"'||r.msg[i]=='\\') esc[ei++]='\\';
        esc[ei++]=r.msg[i];
    }
    esc[ei]='\0';
    snprintf(buf,sizeof(buf),"{\"ok\":%s,\"msg\":\"%s\"}",r.ok?"true":"false",esc);
    send_json(fd, r.ok?200:400, buf);
}
static void serve_file(int fd, const char *path) {
    FILE *f = fopen(path,"rb");
    if (!f) { send_json(fd,404,"{\"error\":\"File not found\"}"); return; }
    fseek(f,0,SEEK_END); long sz=ftell(f); rewind(f);
    char *buf = malloc(sz+1); if(!buf){fclose(f);return;}
    fread(buf,1,sz,f); buf[sz]='\0'; fclose(f);
    http_send(fd,200,"text/html; charset=utf-8",buf,sz);
    free(buf);
}

/* ============================================================
 *  INVENTORY + ORDER JSON BUILDERS
 * ============================================================ */
static void send_inventory(int fd) {
    char *buf = malloc(RESP_SIZE);
    if (!buf) { send_json(fd,500,"{\"error\":\"oom\"}"); return; }
    int pos=0;
    pos+=snprintf(buf+pos,RESP_SIZE-pos,
        "{\"undo_size\":%d,\"queue_size\":%d,\"items\":[",
        g_undo.size, g_orders.size);
    int first=1;
    for(int i=0;i<TABLE_SIZE&&pos<RESP_SIZE-300;i++){
        Item *n=g_table.buckets[i];
        while(n&&pos<RESP_SIZE-300){
            if(!first) pos+=snprintf(buf+pos,RESP_SIZE-pos,",");
            /* Escape name for JSON */
            char en[NAME_LEN*2]; int ei=0;
            for(int j=0;n->name[j]&&ei<(int)sizeof(en)-3;j++){
                if(n->name[j]=='"'||n->name[j]=='\\') en[ei++]='\\';
                en[ei++]=n->name[j];
            }
            en[ei]='\0';
            pos+=snprintf(buf+pos,RESP_SIZE-pos,
                "{\"id\":%d,\"name\":\"%s\",\"quantity\":%d,\"threshold\":%d,\"low\":%s}",
                n->id,en,n->quantity,n->min_threshold,
                n->quantity<=n->min_threshold?"true":"false");
            first=0; n=n->next;
        }
    }
    pos+=snprintf(buf+pos,RESP_SIZE-pos,"]}");
    send_json(fd,200,buf);
    free(buf);
}

static void send_orders(int fd) {
    char buf[4096]; int pos=0;
    pos+=snprintf(buf+pos,sizeof(buf)-pos,"{\"orders\":[");
    int first=1; Order *o=g_orders.front;
    while(o&&pos<(int)sizeof(buf)-100){
        if(!first) pos+=snprintf(buf+pos,sizeof(buf)-pos,",");
        Item *it=ht_search(o->id);
        char en[NAME_LEN*2]=""; 
        if(it){int ei=0;for(int j=0;it->name[j]&&ei<(int)sizeof(en)-3;j++){if(it->name[j]=='"'||it->name[j]=='\\')en[ei++]='\\';en[ei++]=it->name[j];}en[ei]='\0';}
        pos+=snprintf(buf+pos,sizeof(buf)-pos,
            "{\"id\":%d,\"name\":\"%s\",\"quantity\":%d}",
            o->id,en,o->quantity);
        first=0; o=o->next;
    }
    pos+=snprintf(buf+pos,sizeof(buf)-pos,"]}");
    send_json(fd,200,buf);
}

/* ============================================================
 *  REQUEST DISPATCHER
 * ============================================================ */
static void handle(int fd) {
    char *req = malloc(BUF_SIZE);
    if (!req) { close(fd); return; }
    int n = read(fd, req, BUF_SIZE-1);
    if (n<=0) { free(req); return; }
    req[n]='\0';

    /* Parse method + path */
    char method[8]="", path[256]="";
    sscanf(req, "%7s %255s", method, path);

    /* Find body */
    char *body = strstr(req,"\r\n\r\n");
    body = body ? body+4 : req+n;

    /* OPTIONS preflight */
    if (strcmp(method,"OPTIONS")==0) {
        const char *r="HTTP/1.1 204 No Content\r\nAccess-Control-Allow-Origin: *\r\n"
                       "Access-Control-Allow-Methods: GET,POST,OPTIONS\r\n"
                       "Access-Control-Allow-Headers: Content-Type\r\nConnection: close\r\n\r\n";
        write(fd,r,strlen(r)); free(req); return;
    }

    /* Routes */
    if ((strcmp(path,"/")==0||strcmp(path,"/index.html")==0) && strcmp(method,"GET")==0) {
        serve_file(fd,"index.html");
    }
    else if (strcmp(path,"/inventory")==0 && strcmp(method,"GET")==0) {
        send_inventory(fd);
    }
    else if (strcmp(path,"/orders")==0 && strcmp(method,"GET")==0) {
        send_orders(fd);
    }
    else if (strcmp(path,"/add")==0 && strcmp(method,"POST")==0) {
        int id=0,qty=0,thr=0; char name[NAME_LEN]="";
        json_int(body,"id",&id); json_int(body,"quantity",&qty);
        json_int(body,"threshold",&thr); json_str(body,"name",name,NAME_LEN);
        result_json(fd, op_add(id,name,qty,thr));
    }
    else if (strcmp(path,"/delete")==0 && strcmp(method,"POST")==0) {
        int id=0; json_int(body,"id",&id);
        result_json(fd, op_delete(id));
    }
    else if (strcmp(path,"/search")==0 && strcmp(method,"POST")==0) {
        int id=0; json_int(body,"id",&id);
        Result r = op_search(id);
        if (r.ok) send_json(fd,200,r.msg);
        else      result_json(fd,r);
    }
    else if (strcmp(path,"/update")==0 && strcmp(method,"POST")==0) {
        int id=0,delta=0; json_int(body,"id",&id); json_int(body,"delta",&delta);
        result_json(fd, op_update(id,delta));
    }
    else if (strcmp(path,"/order")==0 && strcmp(method,"POST")==0) {
        int id=0,qty=0; json_int(body,"id",&id); json_int(body,"quantity",&qty);
        result_json(fd, op_add_order(id,qty));
    }
    else if (strcmp(path,"/dispatch")==0 && strcmp(method,"POST")==0) {
        result_json(fd, op_dispatch());
    }
    else if (strcmp(path,"/undo")==0 && strcmp(method,"POST")==0) {
        result_json(fd, op_undo());
    }
    else if (strcmp(path,"/export")==0 && strcmp(method,"POST")==0) {
        result_json(fd, op_export());
    }
    else {
        send_json(fd,404,"{\"error\":\"Not found\"}");
    }
    free(req);
}

/* ============================================================
 *  MAIN
 * ============================================================ */
static volatile int g_running = 1;
static void on_sigint(int s) { (void)s; g_running=0; }

int main(void) {
    memset(&g_table,0,sizeof(g_table));
    memset(&g_orders,0,sizeof(g_orders));
    memset(&g_undo,0,sizeof(g_undo));

    signal(SIGINT, on_sigint);
    signal(SIGPIPE, SIG_IGN);   /* ignore broken pipe from browser */

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); return 1; }

    int opt=1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(PORT);

    if (bind(server_fd,(struct sockaddr*)&addr,sizeof(addr))<0) { perror("bind"); return 1; }
    if (listen(server_fd,16)<0) { perror("listen"); return 1; }

    printf("\n  ╔══════════════════════════════════════╗\n");
    printf("  ║  Warehouse IMS — Web Server          ║\n");
    printf("  ║  http://localhost:%-5d               ║\n", PORT);
    printf("  ║  Press Ctrl+C to stop                ║\n");
    printf("  ╚══════════════════════════════════════╝\n\n");

    while (g_running) {
        struct sockaddr_in client; socklen_t clen=sizeof(client);
        int client_fd = accept(server_fd,(struct sockaddr*)&client,&clen);
        if (client_fd < 0) { if(errno==EINTR) break; continue; }
        handle(client_fd);
        close(client_fd);
    }

    close(server_fd);
    cleanup();
    printf("\n  Server stopped. Memory cleaned. Goodbye!\n\n");
    return 0;
}
