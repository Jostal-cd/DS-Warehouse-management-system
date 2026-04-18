/*
 * ============================================================
 *   WAREHOUSE INVENTORY MANAGEMENT SYSTEM
 *   Aligned with SDG 9: Industry, Innovation & Infrastructure
 * ============================================================
 *
 * FINAL SUMMARY:
 * This system integrates four core data structures to manage
 * warehouse inventory and order processing efficiently:
 *
 *   1. Hash Table (with chaining via Linked List)
 *      - Provides O(1) average-case lookup by Item ID.
 *      - Collisions are resolved through linked-list chaining.
 *
 *   2. Linked List
 *      - Backs the hash table's collision chains.
 *      - Also used as the base for the Stack and Queue.
 *
 *   3. Stack (Linked List-based, LIFO)
 *      - Powers the Undo system.
 *      - Stores full action details (type, id, quantity, name,
 *        threshold) so every operation can be fully reversed.
 *
 *   4. Queue (Linked List-based, FIFO)
 *      - Models the order-processing pipeline.
 *      - Orders are dispatched in the sequence they were placed.
 *
 * Real-world edge cases handled:
 *   - Duplicate item IDs, deleting non-existing items.
 *   - Negative / zero stock prevention.
 *   - Order quantity exceeding available stock.
 *   - Low-stock alerts per item threshold.
 *   - Stack underflow (undo when empty).
 *   - Queue underflow (dispatch when empty).
 *   - Invalid (non-integer) console input.
 *   - Multi-word string input via fgets.
 *   - Memory cleanup on exit (no leaks).
 *
 * Business features:
 *   - Low Stock Alert shown on every inventory display.
 *   - CSV Export via fprintf to "inventory.csv".
 *
 * Build:  gcc -Wall -Wextra -o warehouse warehouse_ims.c
 * ============================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ============================================================
 *  CONSTANTS
 * ============================================================ */
#define TABLE_SIZE   53          /* Prime — reduces hash clustering  */
#define NAME_LEN     64          /* Max characters for an item name  */
#define ACTION_ADD   'A'
#define ACTION_DEL   'D'
#define ACTION_UPD   'U'
#define ACTION_ORD   'O'
#define ACTION_DIS   'S'          /* Dispatch — for undo             */

/* ============================================================
 *  STRUCT DEFINITIONS
 * ============================================================ */

/* ── Inventory Item (node in hash-table chain / linked list) ── */
typedef struct Item {
    int          id;
    char         name[NAME_LEN];
    int          quantity;
    int          min_threshold;   /* Low-stock alert level          */
    struct Item *next;            /* Next node in chaining list     */
} Item;

/* ── Order (node in Queue) ── */
typedef struct Order {
    int          id;              /* References an Item id          */
    int          quantity;
    struct Order*next;
} Order;

/* ── Action (node in Undo Stack) ── */
typedef struct Action {
    char          action_type;    /* A / D / U / O                  */
    int           id;
    int           quantity;
    char          name[NAME_LEN]; /* Needed to restore deleted items */
    int           min_threshold;  /* Needed to restore deleted items */
    int           old_quantity;   /* Previous quantity (for Update)  */
    struct Action*next;
} Action;

/* ── Hash Table ── */
typedef struct {
    Item *buckets[TABLE_SIZE];    /* Array of linked-list heads      */
} HashTable;

/* ── Queue (FIFO) ── */
typedef struct {
    Order *front;
    Order *rear;
    int    size;
} Queue;

/* ── Stack (LIFO) ── */
typedef struct {
    Action *top;
    int     size;
} Stack;

/* ============================================================
 *  GLOBAL DATA STRUCTURES
 * ============================================================ */
static HashTable  g_table;        /* Main inventory store            */
static Queue      g_orders;       /* Pending order queue             */
static Stack      g_undo;         /* Undo history stack              */

/* ============================================================
 *  UTILITY HELPERS
 * ============================================================ */

/* Hash function: maps integer id to a bucket index using modular arithmetic */
static int hash(int id) {
    /* Ensure non-negative bucket index */
    int h = id % TABLE_SIZE;
    if (h < 0) h += TABLE_SIZE;
    return h;
}

/* Trim trailing newline / whitespace from fgets buffer */
static void trim_newline(char *s) {
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r'
                       || s[len - 1] == ' '))
        s[--len] = '\0';
}

/* Read a single integer from stdin; return 0 on failure */
static int read_int(const char *prompt, int *out) {
    char buf[32];
    printf("%s", prompt);
    if (!fgets(buf, sizeof(buf), stdin)) return 0;
    /* Check every character is a digit (allow leading '-') */
    int i = 0;
    if (buf[i] == '-') i++;
    if (buf[i] == '\0' || buf[i] == '\n') return 0;
    for (; buf[i] != '\0' && buf[i] != '\n'; i++) {
        if (!isdigit((unsigned char)buf[i])) {
            printf("  [!] Invalid input — please enter an integer.\n");
            return 0;
        }
    }
    *out = atoi(buf);
    return 1;
}

/* Read a string (multi-word) from stdin into dest (max len) */
static void read_string(const char *prompt, char *dest, int len) {
    printf("%s", prompt);
    if (fgets(dest, len, stdin))
        trim_newline(dest);
}

/* ============================================================
 *  HASH TABLE OPERATIONS
 *  Data Structure: Hash Table + Linked List (chaining)
 * ============================================================ */

/* Look up an item by id; returns pointer or NULL */
static Item *ht_search(int id) {
    int   idx  = hash(id);
    Item *node = g_table.buckets[idx];
    while (node) {
        if (node->id == id) return node;
        node = node->next;
    }
    return NULL;
}

/* Insert a new item into the hash table (no duplicate check here) */
static void ht_insert(Item *item) {
    int idx              = hash(item->id);
    item->next           = g_table.buckets[idx]; /* Prepend to chain */
    g_table.buckets[idx] = item;
}

/* Remove item with given id from hash table; returns 1 if found */
static int ht_remove(int id) {
    int   idx  = hash(id);
    Item *prev = NULL;
    Item *cur  = g_table.buckets[idx];
    while (cur) {
        if (cur->id == id) {
            if (prev) prev->next = cur->next;
            else       g_table.buckets[idx] = cur->next;
            free(cur);
            return 1;
        }
        prev = cur;
        cur  = cur->next;
    }
    return 0;
}

/* ============================================================
 *  STACK OPERATIONS  (Undo System)
 *  Data Structure: Linked List-based Stack (LIFO)
 * ============================================================ */

/* Push an action onto the undo stack */
static void stack_push(char type, int id, int qty,
                        const char *name, int threshold, int old_qty) {
    Action *a = (Action *)malloc(sizeof(Action));
    if (!a) { printf("  [!] Memory allocation failed.\n"); return; }
    a->action_type  = type;
    a->id           = id;
    a->quantity     = qty;
    a->min_threshold= threshold;
    a->old_quantity = old_qty;
    strncpy(a->name, name ? name : "", NAME_LEN - 1);
    a->name[NAME_LEN - 1] = '\0';
    a->next    = g_undo.top;
    g_undo.top = a;
    g_undo.size++;
}

/* Pop the top action; returns pointer (caller must free) or NULL */
static Action *stack_pop(void) {
    if (!g_undo.top) return NULL;
    Action *a  = g_undo.top;
    g_undo.top = a->next;
    g_undo.size--;
    a->next = NULL;
    return a;
}

/* ============================================================
 *  QUEUE OPERATIONS  (Order Processing)
 *  Data Structure: Linked List-based Queue (FIFO)
 * ============================================================ */

/* Enqueue an order at the rear */
static void queue_enqueue(int id, int qty) {
    Order *o = (Order *)malloc(sizeof(Order));
    if (!o) { printf("  [!] Memory allocation failed.\n"); return; }
    o->id       = id;
    o->quantity = qty;
    o->next     = NULL;
    if (g_orders.rear) g_orders.rear->next = o;
    else               g_orders.front      = o;
    g_orders.rear = o;
    g_orders.size++;
}

/* Dequeue from the front; returns pointer (caller must free) or NULL */
static Order *queue_dequeue(void) {
    if (!g_orders.front) return NULL;
    Order *o       = g_orders.front;
    g_orders.front = o->next;
    if (!g_orders.front) g_orders.rear = NULL;
    g_orders.size--;
    o->next = NULL;
    return o;
}

/* Enqueue an order at the front of the queue (used by dispatch undo) */
static void queue_enqueue_front(int id, int qty) {
    Order *o = (Order *)malloc(sizeof(Order));
    if (!o) { printf("  [!] Memory allocation failed.\n"); return; }
    o->id       = id;
    o->quantity = qty;
    o->next     = g_orders.front;
    g_orders.front = o;
    if (!g_orders.rear) g_orders.rear = o;  /* was empty */
    g_orders.size++;
}

/* ============================================================
 *  INVENTORY OPERATIONS
 * ============================================================ */

/*
 * addItem() — creates a new Item node and inserts into hash table.
 * Checks for duplicate ID before insertion.
 * Pushes ACTION_ADD onto undo stack.
 */
void addItem(void) {
    int id, qty, thresh;

    printf("\n--- Add Item ---\n");
    if (!read_int("  Item ID        : ", &id)) return;
    if (id <= 0) { printf("  [!] ID must be positive.\n"); return; }
    if (ht_search(id)) {
        printf("  [!] Item ID %d already exists. Use Update Stock instead.\n", id);
        return;
    }
    if (!read_int("  Quantity       : ", &qty)) return;
    if (qty < 0) { printf("  [!] Quantity cannot be negative.\n"); return; }
    if (!read_int("  Min Threshold  : ", &thresh)) return;
    if (thresh < 0) { printf("  [!] Threshold cannot be negative.\n"); return; }

    char name[NAME_LEN];
    read_string("  Item Name      : ", name, NAME_LEN);
    if (strlen(name) == 0) { printf("  [!] Name cannot be empty.\n"); return; }

    /* Allocate and populate item */
    Item *item = (Item *)malloc(sizeof(Item));
    if (!item) { printf("  [!] Memory allocation failed.\n"); return; }
    item->id            = id;
    item->quantity      = qty;
    item->min_threshold = thresh;
    item->next          = NULL;
    strncpy(item->name, name, NAME_LEN - 1);
    item->name[NAME_LEN - 1] = '\0';

    /* Insert into hash table (Data Structure: Hash Table) */
    ht_insert(item);

    /* Record action for undo (Data Structure: Stack) */
    stack_push(ACTION_ADD, id, qty, name, thresh, 0);

    printf("  [+] Item '%s' (ID: %d) added successfully.\n", name, id);

    /* Low stock warning immediately if below threshold */
    if (qty <= thresh)
        printf("  [!] WARNING: Stock (%d) is at or below threshold (%d)!\n",
               qty, thresh);
}

/*
 * deleteItem() — removes an item from the hash table.
 * Validates existence. Saves full item details to undo stack
 * so the item can be completely restored on undo.
 */
void deleteItem(void) {
    int id;
    printf("\n--- Delete Item ---\n");
    if (!read_int("  Item ID to delete : ", &id)) return;

    Item *item = ht_search(id);
    if (!item) {
        printf("  [!] Item ID %d not found.\n", id);
        return;
    }

    /* Save details before removal (for undo restoration) */
    stack_push(ACTION_DEL, id, item->quantity,
               item->name, item->min_threshold, 0);

    ht_remove(id);   /* frees the node */
    printf("  [-] Item ID %d deleted successfully.\n", id);
}

/*
 * updateStock() — modifies quantity of an existing item.
 * Prevents negative resulting stock.
 * Records old quantity on undo stack.
 */
void updateStock(void) {
    int id, delta;
    printf("\n--- Update Stock ---\n");
    if (!read_int("  Item ID  : ", &id)) return;

    Item *item = ht_search(id);
    if (!item) { printf("  [!] Item ID %d not found.\n", id); return; }

    printf("  Current stock : %d\n", item->quantity);
    if (!read_int("  Change amount (negative to reduce) : ", &delta)) return;

    int new_qty = item->quantity + delta;
    if (new_qty < 0) {
        printf("  [!] Operation would result in negative stock (%d). Aborted.\n",
               new_qty);
        return;
    }

    /* Push old state for undo */
    stack_push(ACTION_UPD, id, delta, item->name,
               item->min_threshold, item->quantity);

    item->quantity = new_qty;
    printf("  [~] Stock updated: %d -> %d\n",
           new_qty - delta, new_qty);

    if (new_qty <= item->min_threshold)
        printf("  [!] WARNING: Stock (%d) is at or below threshold (%d)!\n",
               new_qty, item->min_threshold);
}

/*
 * searchItem() — performs fast hash-table lookup.
 * Data Structure: Hash Table
 */
void searchItem(void) {
    int id;
    printf("\n--- Search Item ---\n");
    if (!read_int("  Item ID to search : ", &id)) return;

    Item *item = ht_search(id);
    if (!item) {
        printf("  [!] Item ID %d not found.\n", id);
        return;
    }
    printf("  ┌──────────────────────────────┐\n");
    printf("  │ ID        : %-17d│\n", item->id);
    printf("  │ Name      : %-17s│\n", item->name);
    printf("  │ Quantity  : %-17d│\n", item->quantity);
    printf("  │ Threshold : %-17d│\n", item->min_threshold);
    printf("  └──────────────────────────────┘\n");

    if (item->quantity <= item->min_threshold)
        printf("  [!] WARNING: Low stock alert for '%s'!\n", item->name);
}

/*
 * displayInventory() — iterates all hash-table buckets and
 * prints every item. Flags low-stock items.
 */
void displayInventory(void) {
    printf("\n============================================================\n");
    printf("                   WAREHOUSE INVENTORY\n");
    printf("============================================================\n");
    printf("  %-6s %-20s %-10s %-10s %s\n",
           "ID", "Name", "Quantity", "Threshold", "Status");
    printf("  %-6s %-20s %-10s %-10s %s\n",
           "------", "--------------------", "--------", "---------", "------");

    int found = 0;
    for (int i = 0; i < TABLE_SIZE; i++) {
        Item *node = g_table.buckets[i];
        while (node) {
            const char *status = (node->quantity <= node->min_threshold)
                                 ? "[LOW STOCK]" : "OK";
            printf("  %-6d %-20s %-10d %-10d %s\n",
                   node->id, node->name,
                   node->quantity, node->min_threshold, status);
            found = 1;
            node = node->next;
        }
    }
    if (!found)
        printf("  (Inventory is empty)\n");

    printf("============================================================\n");
}

/* ============================================================
 *  ORDER MANAGEMENT
 * ============================================================ */

/*
 * addOrder() — validates item existence and stock availability,
 * then enqueues the order.
 * Data Structure: Queue (enqueue)
 */
void addOrder(void) {
    int id, qty;
    printf("\n--- Add Order ---\n");
    if (!read_int("  Item ID  : ", &id)) return;
    if (id <= 0) { printf("  [!] ID must be positive.\n"); return; }
    if (!read_int("  Quantity : ", &qty)) return;
    if (qty <= 0) { printf("  [!] Order quantity must be positive.\n"); return; }

    Item *item = ht_search(id);
    if (!item) { printf("  [!] Item ID %d not found.\n", id); return; }
    if (item->quantity < qty) {
        printf("  [!] Insufficient stock. Available: %d, Requested: %d\n",
               item->quantity, qty);
        return;
    }

    queue_enqueue(id, qty);

    /* Record order action for undo */
    stack_push(ACTION_ORD, id, qty, item->name, item->min_threshold, 0);

    printf("  [>] Order placed — Item ID %d, Qty %d (Queue size: %d)\n",
           id, qty, g_orders.size);
}

/*
 * dispatchOrder() — dequeues the front order (FIFO),
 * reduces item stock, and prints dispatch summary.
 * Data Structure: Queue (dequeue)
 */
void dispatchOrder(void) {
    printf("\n--- Dispatch Order ---\n");
    if (!g_orders.front) {
        printf("  [!] Order queue is empty. Nothing to dispatch.\n");
        return;
    }

    Order *o    = queue_dequeue();
    Item  *item = ht_search(o->id);

    if (!item) {
        printf("  [!] Item ID %d no longer exists. Order cancelled.\n", o->id);
        free(o);
        return;
    }
    if (item->quantity < o->quantity) {
        printf("  [!] Insufficient stock to dispatch "
               "(need %d, have %d). Order cancelled.\n",
               o->quantity, item->quantity);
        free(o);
        return;
    }

    int dispatched_qty  = o->quantity;
    item->quantity     -= dispatched_qty;

    /* Record dispatch for undo (Data Structure: Stack) */
    stack_push(ACTION_DIS, item->id, dispatched_qty,
               item->name, item->min_threshold, 0);

    printf("  [>>] Dispatched: Item '%s' (ID %d), Qty %d. "
           "Remaining stock: %d\n",
           item->name, item->id, dispatched_qty, item->quantity);

    if (item->quantity <= item->min_threshold)
        printf("  [!] WARNING: Low stock for '%s' after dispatch!\n",
               item->name);

    free(o);
}

/* ============================================================
 *  UNDO SYSTEM
 *  Data Structure: Stack (pop / reverse action)
 * ============================================================ */

/*
 * undoOperation() — pops the last action from the undo stack
 * and reverses it precisely.
 */
void undoOperation(void) {
    printf("\n--- Undo Last Operation ---\n");

    Action *a = stack_pop();
    if (!a) {
        printf("  [!] Nothing to undo (stack is empty).\n");
        return;
    }

    switch (a->action_type) {

    case ACTION_ADD:
        /* Reverse ADD → delete the item that was added */
        if (ht_remove(a->id))
            printf("  [<] Undo ADD: Item ID %d removed.\n", a->id);
        else
            printf("  [!] Undo ADD: Item ID %d not found (already deleted?).\n",
                   a->id);
        break;

    case ACTION_DEL:
        /* Reverse DELETE → re-insert the item with saved details */
        if (ht_search(a->id)) {
            printf("  [!] Undo DEL: Item ID %d already exists.\n", a->id);
        } else {
            Item *item = (Item *)malloc(sizeof(Item));
            if (!item) { printf("  [!] Memory error.\n"); break; }
            item->id            = a->id;
            item->quantity      = a->quantity;
            item->min_threshold = a->min_threshold;
            item->next          = NULL;
            strncpy(item->name, a->name, NAME_LEN - 1);
            item->name[NAME_LEN - 1] = '\0';
            ht_insert(item);
            printf("  [<] Undo DEL: Item '%s' (ID %d) restored.\n",
                   a->name, a->id);
        }
        break;

    case ACTION_UPD:
        /* Reverse UPDATE → restore old quantity */
        {
            Item *item = ht_search(a->id);
            if (!item) {
                printf("  [!] Undo UPD: Item ID %d not found.\n", a->id);
            } else {
                printf("  [<] Undo UPDATE: Stock of '%s' restored from %d to %d.\n",
                       item->name, item->quantity, a->old_quantity);
                item->quantity = a->old_quantity;
            }
        }
        break;

    case ACTION_ORD:
        /* Reverse ORDER → remove the last-placed order from the queue rear.
         * The last placed order is always at g_orders.rear (FIFO). */
        {
            Order *cur = g_orders.rear;
            if (!cur) {
                printf("  [!] Undo ORDER: Queue is empty.\n");
                break;
            }
            if (cur->id == a->id && cur->quantity == a->quantity) {
                if (g_orders.front == g_orders.rear) {
                    /* Only one order in the queue */
                    g_orders.front = g_orders.rear = NULL;
                } else {
                    /* Traverse to find the node just before rear */
                    Order *prev = g_orders.front;
                    while (prev->next != cur) prev = prev->next;
                    prev->next    = NULL;
                    g_orders.rear = prev;
                }
                g_orders.size--;
                free(cur);
                printf("  [<] Undo ORDER: Order for Item ID %d (Qty %d) removed.\n",
                       a->id, a->quantity);
            } else {
                printf("  [!] Undo ORDER: Matching order not found at queue rear.\n");
            }
        }
        break;

    case ACTION_DIS:
        /* Reverse DISPATCH → restore stock and re-insert order at queue front */
        {
            Item *item = ht_search(a->id);
            if (!item) {
                printf("  [!] Undo DISPATCH: Item ID %d not found.\n", a->id);
            } else {
                item->quantity += a->quantity;
                queue_enqueue_front(a->id, a->quantity);
                printf("  [<] Undo DISPATCH: Stock of '%s' restored by %d."
                       " Order re-queued at front.\n",
                       item->name, a->quantity);
            }
        }
        break;

    default:
        printf("  [!] Unknown action type '%c'.\n", a->action_type);
    }

    free(a);
}

/* ============================================================
 *  CSV EXPORT
 * ============================================================ */

/*
 * exportCSV() — writes all inventory items to "inventory.csv"
 * using fprintf for data persistence.
 */
void exportCSV(void) {
    printf("\n--- Export to CSV ---\n");

    FILE *fp = fopen("inventory.csv", "w");
    if (!fp) {
        printf("  [!] Could not open inventory.csv for writing.\n");
        return;
    }

    /* Write header row */
    fprintf(fp, "ID,Name,Quantity,MinThreshold,Status\n");

    int count = 0;
    for (int i = 0; i < TABLE_SIZE; i++) {
        Item *node = g_table.buckets[i];
        while (node) {
            const char *status = (node->quantity <= node->min_threshold)
                                 ? "LOW" : "OK";
            /* Quote name field to handle commas in item names */
            fprintf(fp, "%d,\"%s\",%d,%d,%s\n",
                    node->id, node->name,
                    node->quantity, node->min_threshold, status);
            count++;
            node = node->next;
        }
    }

    fclose(fp);
    printf("  [CSV] Exported %d item(s) to inventory.csv\n", count);
}

/* ============================================================
 *  MEMORY CLEANUP
 * ============================================================ */

/* Free all items in the hash table */
static void cleanup_table(void) {
    for (int i = 0; i < TABLE_SIZE; i++) {
        Item *node = g_table.buckets[i];
        while (node) {
            Item *tmp = node->next;
            free(node);
            node = tmp;
        }
        g_table.buckets[i] = NULL;
    }
}

/* Free all orders in the queue */
static void cleanup_queue(void) {
    Order *node = g_orders.front;
    while (node) {
        Order *tmp = node->next;
        free(node);
        node = tmp;
    }
    g_orders.front = g_orders.rear = NULL;
    g_orders.size  = 0;
}

/* Free all actions in the undo stack */
static void cleanup_stack(void) {
    Action *node = g_undo.top;
    while (node) {
        Action *tmp = node->next;
        free(node);
        node = tmp;
    }
    g_undo.top  = NULL;
    g_undo.size = 0;
}

/* ============================================================
 *  MENU
 * ============================================================ */

static void print_menu(void) {
    printf(" WAREHOUSE INVENTORY MANAGER\n");

    printf("1.  Add Item\n");
    printf("2.  Delete Item\n");
    printf("3.  Search Item\n");
    printf("4.  Update Stock\n");
    printf("5.  Add Order\n");
    printf("6.  Dispatch Order\n");
    printf("7.  Undo Last Operation\n");
    printf("8.  Display Inventory\n");
    printf("9.  Export to CSV\n");
    printf("10. Exit\n");
}

/* ============================================================
 *  MAIN
 * ============================================================ */
int main(void) {
    /* Initialise all structures to zero/NULL */
    memset(&g_table,  0, sizeof(g_table));
    memset(&g_orders, 0, sizeof(g_orders));
    memset(&g_undo,   0, sizeof(g_undo));

    printf("\n  Welcome to the Warehouse Inventory Management System\n");

    int choice = 0;
    while (1) {
        print_menu();
        if (!read_int("  Enter choice : ", &choice)) {
            printf("  [!] Please enter a number between 1 and 10.\n");
            continue;
        }

        switch (choice) {
        case 1:  addItem();         break;
        case 2:  deleteItem();      break;
        case 3:  searchItem();      break;
        case 4:  updateStock();     break;
        case 5:  addOrder();        break;
        case 6:  dispatchOrder();   break;
        case 7:  undoOperation();   break;
        case 8:  displayInventory();break;
        case 9:  exportCSV();       break;
        case 10:
            printf("\n  Cleaning up memory and exiting. Goodbye!\n\n");
            cleanup_table();
            cleanup_queue();
            cleanup_stack();
            return 0;
        default:
            printf("  [!] Invalid choice. Enter 1-10.\n");
        }
    }
}