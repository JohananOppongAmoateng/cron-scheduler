#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct heap_item {
    char next_run[20];
    char job_id[128];
};

struct min_heap {
    struct heap_item* items;
    size_t size;
    size_t capacity;
};

static int valid_iso_datetime(const char* text) {
    if (strlen(text) != 19 ||
        text[4] != '-' || text[7] != '-' || text[10] != 'T' ||
        text[13] != ':' || text[16] != ':') {
        return 0;
    }
    for (int i = 0; i < 19; i++) {
        if (i == 4 || i == 7 || i == 10 || i == 13 || i == 16) continue;
        if (!isdigit((unsigned char)text[i])) return 0;
    }
    return 1;
}

static int item_compare(const struct heap_item* left,
                        const struct heap_item* right) {
    int by_time = strcmp(left->next_run, right->next_run);
    if (by_time != 0) return by_time;
    return strcmp(left->job_id, right->job_id);
}

static void swap_items(struct heap_item* left, struct heap_item* right) {
    struct heap_item temporary = *left;
    *left = *right;
    *right = temporary;
}

static int heap_push(struct min_heap* heap, const struct heap_item* item) {
    if (heap->size == heap->capacity) {
        size_t new_capacity = heap->capacity ? heap->capacity * 2 : 8;
        struct heap_item* resized =
            realloc(heap->items, new_capacity * sizeof *heap->items);
        if (!resized) return 0;
        heap->items = resized;
        heap->capacity = new_capacity;
    }

    size_t index = heap->size++;
    heap->items[index] = *item;
    while (index > 0) {
        size_t parent = (index - 1) / 2;
        if (item_compare(&heap->items[parent], &heap->items[index]) <= 0) break;
        swap_items(&heap->items[parent], &heap->items[index]);
        index = parent;
    }
    return 1;
}

static struct heap_item heap_pop(struct min_heap* heap) {
    struct heap_item result = heap->items[0];
    heap->size--;
    if (heap->size == 0) return result;

    heap->items[0] = heap->items[heap->size];
    size_t index = 0;
    for (;;) {
        size_t left = index * 2 + 1;
        size_t right = left + 1;
        size_t smallest = index;

        if (left < heap->size &&
            item_compare(&heap->items[left], &heap->items[smallest]) < 0) {
            smallest = left;
        }
        if (right < heap->size &&
            item_compare(&heap->items[right], &heap->items[smallest]) < 0) {
            smallest = right;
        }
        if (smallest == index) break;
        swap_items(&heap->items[index], &heap->items[smallest]);
        index = smallest;
    }
    return result;
}

static void print_item(const struct heap_item* item) {
    printf("%s %s\n", item->next_run, item->job_id);
}

int main(void) {
    struct min_heap heap = {0};
    char line[512];

    while (fgets(line, sizeof line, stdin)) {
        line[strcspn(line, "\r\n")] = '\0';

        if (strncmp(line, "PUSH", 4) == 0 &&
            isspace((unsigned char)line[4])) {
            struct heap_item item;
            char trailing;
            int parsed = sscanf(line, "PUSH %19s %127s %c",
                                item.next_run, item.job_id, &trailing);
            if (parsed != 2 || !valid_iso_datetime(item.next_run) ||
                !heap_push(&heap, &item)) {
                printf("ERR\n");
            } else {
                printf("OK\n");
            }
        } else if (strcmp(line, "PEEK") == 0) {
            if (heap.size == 0) printf("EMPTY\n");
            else print_item(&heap.items[0]);
        } else if (strcmp(line, "POP") == 0) {
            if (heap.size == 0) {
                printf("EMPTY\n");
            } else {
                struct heap_item item = heap_pop(&heap);
                print_item(&item);
            }
        } else if (strcmp(line, "SIZE") == 0) {
            printf("%zu\n", heap.size);
        } else {
            printf("ERR\n");
        }
    }

    free(heap.items);
    return 0;
}
