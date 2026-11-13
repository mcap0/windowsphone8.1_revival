/*
 * armprof — a qemu-arm (linux-user) TCG plugin: how many guest instructions each translated block executed, all threads
 * together, written at exit as "pc instructions executions" lines into the file named by out=PATH, after a header with
 * the program's code segment (start_code end_code) and the executable mappings of files the program made (the shared
 * libraries: "map start end file-offset path"; executable memory without a file is JIT code). armprof.py turns it into a
 * profile by function.
 * Built by armprof.py against the qemu sources of engine/out/qemu-plugin (PLAN.md section 77).
 */
#include <glib.h>
#include <inttypes.h>
#include <qemu-plugin.h>
#include <stdio.h>
#include <string.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

typedef struct {
    uint64_t pc;
    unsigned long insns;
    struct qemu_plugin_scoreboard* executions;
} Block;

static GMutex lock;
static GHashTable* blocks;
static char* outPath;
// Syscalls of 32-bit ARM EABI Linux.
enum { SysClose = 6, SysOpen = 5, SysOpenat = 322, SysMmap2 = 192 };
enum { ProtExec = 4 };
static GHashTable* fdPaths; // fd -> path
static GString* maps;
static __thread char pendingPath[512];
static __thread uint64_t pendingMmap[6];
static __thread int pendingClose = -1;

static void syscallEntry(qemu_plugin_id_t id, unsigned int vcpu, int64_t number, uint64_t a1, uint64_t a2, uint64_t a3,
    uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7, uint64_t a8)
{
    if (number == SysOpenat || number == SysOpen) {
        uint64_t address = number == SysOpenat ? a2 : a1;
        GByteArray* data = g_byte_array_new();
        pendingPath[0] = 0;
        // The path, a piece at a time (a read across an unmapped page fails as a whole).
        for (unsigned offset = 0; offset < sizeof(pendingPath) - 1; offset += 16) {
            g_byte_array_set_size(data, 0);
            if (!qemu_plugin_read_memory_vaddr(address + offset, data, 16))
                break;
            void* end = memchr(data->data, 0, 16);
            size_t length = end ? (size_t)((uint8_t*)end - data->data) : 16;
            if (offset + length >= sizeof(pendingPath) - 1)
                break;
            memcpy(pendingPath + offset, data->data, length);
            pendingPath[offset + length] = 0;
            if (end)
                break;
        }
        g_byte_array_free(data, TRUE);
    } else if (number == SysClose)
        pendingClose = (int)a1;
    else if (number == SysMmap2) {
        uint64_t args[6] = { a1, a2, a3, a4, a5, a6 };
        memcpy(pendingMmap, args, sizeof(args));
    }
}

static void syscallReturn(qemu_plugin_id_t id, unsigned int vcpu, int64_t number, int64_t result)
{
    g_mutex_lock(&lock);
    if ((number == SysOpenat || number == SysOpen) && result >= 0 && pendingPath[0])
        g_hash_table_insert(fdPaths, GINT_TO_POINTER((int)result), g_strdup(pendingPath));
    else if (number == SysClose)
        g_hash_table_remove(fdPaths, GINT_TO_POINTER(pendingClose));
    else if (number == SysMmap2 && (pendingMmap[2] & ProtExec) && (int32_t)pendingMmap[4] >= 0 && (uint32_t)result < 0xfffff000u) {
        const char* path = g_hash_table_lookup(fdPaths, GINT_TO_POINTER((int)(int32_t)pendingMmap[4]));
        if (path)
            g_string_append_printf(maps, "map %" PRIx64 " %" PRIx64 " %" PRIx64 " %s\n", (uint64_t)(uint32_t)result,
                (uint64_t)(uint32_t)result + (uint32_t)pendingMmap[1], (uint64_t)(uint32_t)pendingMmap[5] * 4096, path);
    }
    g_mutex_unlock(&lock);
}

static guint blockHash(gconstpointer v)
{
    const Block* b = v;
    return (guint)(b->pc ^ (b->pc >> 32) ^ b->insns);
}

static gboolean blockEqual(gconstpointer a, gconstpointer b)
{
    const Block* x = a;
    const Block* y = b;
    return x->pc == y->pc && x->insns == y->insns;
}

static void translate(qemu_plugin_id_t id, struct qemu_plugin_tb* tb)
{
    Block key = { qemu_plugin_tb_vaddr(tb), qemu_plugin_tb_n_insns(tb), NULL };
    g_mutex_lock(&lock);
    Block* block = g_hash_table_lookup(blocks, &key);
    if (!block) {
        block = g_new0(Block, 1);
        *block = key;
        block->executions = qemu_plugin_scoreboard_new(sizeof(uint64_t));
        g_hash_table_insert(blocks, block, block);
    }
    g_mutex_unlock(&lock);
    qemu_plugin_register_vcpu_tb_exec_inline_per_vcpu(tb, QEMU_PLUGIN_INLINE_ADD_U64,
        qemu_plugin_scoreboard_u64(block->executions), 1);
}

static void writeOne(gpointer key, gpointer value, gpointer data)
{
    Block* block = value;
    uint64_t executions = qemu_plugin_u64_sum(qemu_plugin_scoreboard_u64(block->executions));
    if (executions)
        fprintf(data, "%" PRIx64 " %lu %" PRIu64 "\n", block->pc, block->insns, executions);
}

static void finish(qemu_plugin_id_t id, void* p)
{
    FILE* out = fopen(outPath ? outPath : "armprof.txt", "w");
    if (!out)
        return;
    fprintf(out, "code %" PRIx64 " %" PRIx64 "\n", qemu_plugin_start_code(), qemu_plugin_end_code());
    fputs(maps->str, out);
    g_hash_table_foreach(blocks, writeOne, out);
    fclose(out);
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id, const qemu_info_t* info, int argc, char** argv)
{
    for (int i = 0; i < argc; i++) {
        if (!strncmp(argv[i], "out=", 4))
            outPath = g_strdup(argv[i] + 4);
    }
    blocks = g_hash_table_new(blockHash, blockEqual);
    fdPaths = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
    maps = g_string_new(NULL);
    qemu_plugin_register_vcpu_syscall_cb(id, syscallEntry);
    qemu_plugin_register_vcpu_syscall_ret_cb(id, syscallReturn);
    qemu_plugin_register_vcpu_tb_trans_cb(id, translate);
    qemu_plugin_register_atexit_cb(id, finish, NULL);
    return 0;
}
