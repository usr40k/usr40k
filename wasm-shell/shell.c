/*
 * usr40k web-shell — a real shell compiled to WebAssembly.
 *
 * Built with clang targeting wasm32. No libc, no WASI — everything
 * is implemented from scratch so the module is self-contained and
 * runs in any browser.
 *
 * Exports:
 *   void  shell_init(void)
 *   int   shell_exec(const char* cmd, int cmd_len)
 *   char* shell_get_output(void)
 *   int   shell_get_output_len(void)
 *   void* shell_alloc(int size)
 *   void  shell_free(void* ptr)
 */

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef int i32;
typedef unsigned int usize;

/* ------------------------------------------------------------------ */
/* Memory allocator — simple free-list allocator                       */
/* ------------------------------------------------------------------ */

#define HEAP_SIZE (1 << 20) /* 1 MiB heap */

static u8 heap[HEAP_SIZE] __attribute__((aligned(16)));

typedef struct Block {
    usize size;
    struct Block* next;
    int free;
} Block;

static Block* heap_start = 0;

static void mem_init(void) {
    if (heap_start) return;
    heap_start = (Block*)heap;
    heap_start->size = HEAP_SIZE - sizeof(Block);
    heap_start->next = 0;
    heap_start->free = 1;
}

void* shell_alloc(int size) {
    mem_init();
    if (size <= 0) return 0;
    usize need = (usize)size;
    /* align to 16 */
    need = (need + 15) & ~(usize)15;

    Block* cur = heap_start;
    while (cur) {
        if (cur->free && cur->size >= need) {
            /* split if large enough */
            if (cur->size > need + sizeof(Block) + 16) {
                Block* new_block = (Block*)((u8*)cur + sizeof(Block) + need);
                new_block->size = cur->size - need - sizeof(Block);
                new_block->next = cur->next;
                new_block->free = 1;
                cur->next = new_block;
                cur->size = need;
            }
            cur->free = 0;
            return (void*)((u8*)cur + sizeof(Block));
        }
        cur = cur->next;
    }
    return 0; /* out of memory */
}

void shell_free(void* ptr) {
    if (!ptr) return;
    Block* blk = (Block*)((u8*)ptr - sizeof(Block));
    blk->free = 1;
    /* coalesce with next */
    Block* cur = blk;
    while (cur->next && cur->next->free) {
        cur->size += sizeof(Block) + cur->next->size;
        cur->next = cur->next->next;
    }
}

/* ------------------------------------------------------------------ */
/* String utilities                                                    */
/* ------------------------------------------------------------------ */

static usize str_len(const char* s) {
    usize n = 0;
    while (s[n]) n++;
    return n;
}

static int str_cmp(const char* a, const char* b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return (u8)*a - (u8)*b;
}

static int str_ncmp(const char* a, const char* b, usize n) {
    for (usize i = 0; i < n; i++) {
        if (a[i] != b[i]) return (u8)a[i] - (u8)b[i];
        if (!a[i]) return 0;
    }
    return 0;
}

static void str_cpy(char* dst, const char* src) {
    while ((*dst++ = *src++));
}

static void str_ncpy(char* dst, const char* src, usize n) {
    for (usize i = 0; i < n && src[i]; i++) dst[i] = src[i];
    dst[n] = 0;
}

static void str_cat(char* dst, const char* src) {
    dst += str_len(dst);
    str_cpy(dst, src);
}

static int is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static int is_digit(char c) {
    return c >= '0' && c <= '9';
}

static int is_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static int is_alnum(char c) {
    return is_alpha(c) || is_digit(c);
}

static int to_lower(char c) {
    if (c >= 'A' && c <= 'Z') return c + 32;
    return c;
}

static int to_upper(char c) {
    if (c >= 'a' && c <= 'z') return c - 32;
    return c;
}

static int str_to_int(const char* s) {
    int n = 0;
    int neg = 0;
    if (*s == '-') { neg = 1; s++; }
    while (is_digit(*s)) {
        n = n * 10 + (*s - '0');
        s++;
    }
    return neg ? -n : n;
}

static void int_to_str(int n, char* buf) {
    char tmp[16];
    int i = 0;
    int neg = 0;
    if (n < 0) { neg = 1; n = -n; }
    if (n == 0) { buf[0] = '0'; buf[1] = 0; return; }
    while (n > 0) {
        tmp[i++] = '0' + (n % 10);
        n /= 10;
    }
    int j = 0;
    if (neg) buf[j++] = '-';
    while (i > 0) buf[j++] = tmp[--i];
    buf[j] = 0;
}

/* ------------------------------------------------------------------ */
/* Output buffer                                                       */
/* ------------------------------------------------------------------ */

#define OUT_CAP (1 << 16) /* 64 KiB output buffer */

static char out_buf[OUT_CAP];
static usize out_len = 0;

static void out_reset(void) {
    out_len = 0;
    out_buf[0] = 0;
}

static void out_write(const char* s) {
    usize n = str_len(s);
    for (usize i = 0; i < n && out_len < OUT_CAP - 1; i++) {
        out_buf[out_len++] = s[i];
    }
    out_buf[out_len] = 0;
}

static void out_write_char(char c) {
    if (out_len < OUT_CAP - 1) {
        out_buf[out_len++] = c;
        out_buf[out_len] = 0;
    }
}

static void out_write_int(int n) {
    char buf[16];
    int_to_str(n, buf);
    out_write(buf);
}

static void out_write_hex(u32 n) {
    char buf[12];
    int i = 0;
    if (n == 0) { out_write("0x0"); return; }
    while (n > 0) {
        int d = n & 0xF;
        buf[i++] = d < 10 ? '0' + d : 'a' + (d - 10);
        n >>= 4;
    }
    out_write("0x");
    while (i > 0) out_write_char(buf[--i]);
}

/* ------------------------------------------------------------------ */
/* Virtual filesystem                                                  */
/* ------------------------------------------------------------------ */

#define MAX_FILES 64
#define MAX_PATH 256
#define MAX_NAME 64
#define MAX_CONTENT 4096

typedef struct {
    char name[MAX_NAME];
    char path[MAX_PATH];
    int is_dir;
    char content[MAX_CONTENT];
    usize content_len;
    int perm; /* 0 = rw, 1 = ro */
} FSNode;

static FSNode fs_nodes[MAX_FILES];
static int fs_count = 0;
static char cwd[MAX_PATH] = "/home/usr40k/workspace";

static void fs_init(void) {
    fs_count = 0;

    /* root */
    FSNode* n = &fs_nodes[fs_count++];
    str_cpy(n->name, "/");
    str_cpy(n->path, "/");
    n->is_dir = 1;
    n->content_len = 0;
    n->perm = 0;

    /* /home */
    n = &fs_nodes[fs_count++];
    str_cpy(n->name, "home");
    str_cpy(n->path, "/home");
    n->is_dir = 1;
    n->content_len = 0;
    n->perm = 0;

    /* /home/usr40k */
    n = &fs_nodes[fs_count++];
    str_cpy(n->name, "usr40k");
    str_cpy(n->path, "/home/usr40k");
    n->is_dir = 1;
    n->content_len = 0;
    n->perm = 0;

    /* /home/usr40k/workspace */
    n = &fs_nodes[fs_count++];
    str_cpy(n->name, "workspace");
    str_cpy(n->path, "/home/usr40k/workspace");
    n->is_dir = 1;
    n->content_len = 0;
    n->perm = 0;

    /* .dumshrc */
    n = &fs_nodes[fs_count++];
    str_cpy(n->name, ".dumshrc");
    str_cpy(n->path, "/home/usr40k/workspace/.dumshrc");
    n->is_dir = 0;
    n->perm = 1;
    str_cpy(n->content,
        "# dumsh web-shell dumshrc\n"
        "export dumsh=\"$HOME/.oh-my-dumsh\"\n"
        "export PATH=\"$HOME/.local/bin:$PATH\"\n"
        "export TERM=\"xterm-256color\"\n"
        "\n"
        "plugins=(git gitfast dumsh-interactive-cd colored-man-pages)\n"
        "\n"
        "alias ll='ls -al'\n"
        "alias la='ls -A'\n"
        "alias l='ls -CF'\n"
        "\n"
        "PROMPT='usr40k@dumsh:%~$ '\n");
    n->content_len = str_len(n->content);

    /* README.md */
    n = &fs_nodes[fs_count++];
    str_cpy(n->name, "README.md");
    str_cpy(n->path, "/home/usr40k/workspace/README.md");
    n->is_dir = 0;
    n->perm = 1;
    str_cpy(n->content,
        "# usr40k // cybercraft\n"
        "\n"
        "Salutations! I'm usr40k — a tinkerer, script-wrangler,\n"
        "and web experimenter. I build tiny tools, strange\n"
        "interfaces, and playful software that feels like a\n"
        "terminal had a dream.\n"
        "\n"
        "## Projects\n"
        "- Doofus Dot Ultra\n"
        "- megaminer NG\n"
        "- g510s\n"
        "- streamonline\n"
        "\n"
        "## Links\n"
        "- GitHub: https://github.com/usr40k\n"
        "- Matrix: @usr40k:usr40k.dev\n");
    n->content_len = str_len(n->content);

    /* hello.txt */
    n = &fs_nodes[fs_count++];
    str_cpy(n->name, "hello.txt");
    str_cpy(n->path, "/home/usr40k/workspace/hello.txt");
    n->is_dir = 0;
    n->perm = 0;
    str_cpy(n->content,
        "Hello, world!\n"
        "This is a real file in a real WASM filesystem.\n"
        "Try: cat hello.txt\n"
        "Try: grep wasm hello.txt\n");
    n->content_len = str_len(n->content);

    /* projects.txt */
    n = &fs_nodes[fs_count++];
    str_cpy(n->name, "projects.txt");
    str_cpy(n->path, "/home/usr40k/workspace/projects.txt");
    n->is_dir = 0;
    n->perm = 0;
    str_cpy(n->content,
        "Doofus Dot Ultra — chaotic browser toy\n"
        "megaminer NG — browser game\n"
        "g510s — keyboard mods\n"
        "streamonline — terminal livestream checker\n"
        "web-shell — this WASM shell\n");
    n->content_len = str_len(n->content);

    /* /home/usr40k/workspace/projects */
    n = &fs_nodes[fs_count++];
    str_cpy(n->name, "projects");
    str_cpy(n->path, "/home/usr40k/workspace/projects");
    n->is_dir = 1;
    n->content_len = 0;
    n->perm = 0;

    /* /home/usr40k/workspace/docs */
    n = &fs_nodes[fs_count++];
    str_cpy(n->name, "docs");
    str_cpy(n->path, "/home/usr40k/workspace/docs");
    n->is_dir = 1;
    n->content_len = 0;
    n->perm = 0;

    /* docs/notes.txt */
    n = &fs_nodes[fs_count++];
    str_cpy(n->name, "notes.txt");
    str_cpy(n->path, "/home/usr40k/workspace/docs/notes.txt");
    n->is_dir = 0;
    n->perm = 0;
    str_cpy(n->content,
        "WASM shell notes:\n"
        "- Compiled with clang --target=wasm32\n"
        "- No libc, no WASI, fully self-contained\n"
        "- Virtual filesystem in linear memory\n"
        "- Commands: ls, cat, echo, cd, pwd, mkdir, touch, rm, grep, wc, head, tail, date, uname, neofetch, whoami, help, clear, history, env, export\n");
    n->content_len = str_len(n->content);
}

static FSNode* fs_find(const char* path) {
    for (int i = 0; i < fs_count; i++) {
        if (str_cmp(fs_nodes[i].path, path) == 0) {
            return &fs_nodes[i];
        }
    }
    return 0;
}

static FSNode* fs_find_child(const char* dir_path, const char* name) {
    for (int i = 0; i < fs_count; i++) {
        if (fs_nodes[i].is_dir) continue;
        /* check parent path */
        usize plen = str_len(dir_path);
        if (str_ncmp(fs_nodes[i].path, dir_path, plen) == 0 &&
            fs_nodes[i].path[plen] == '/' &&
            str_cmp(fs_nodes[i].name, name) == 0) {
            return &fs_nodes[i];
        }
    }
    return 0;
}

static void fs_resolve(const char* input, char* out) {
    /* resolve relative or absolute path */
    if (input[0] == '/') {
        str_cpy(out, input);
    } else {
        str_cpy(out, cwd);
        if (str_cmp(input, ".") != 0) {
            if (str_cmp(input, "..") == 0) {
                /* go up one level */
                usize len = str_len(out);
                while (len > 0 && out[len - 1] != '/') len--;
                if (len > 0) out[len - 1] = 0;
                if (out[0] == 0) str_cpy(out, "/");
            } else {
                str_cat(out, "/");
                str_cat(out, input);
            }
        }
    }
    /* normalize: remove trailing slash except root */
    usize len = str_len(out);
    while (len > 1 && out[len - 1] == '/') {
        out[len - 1] = 0;
        len--;
    }
}

static void fs_list_dir(const char* dir_path) {
    int found = 0;
    for (int i = 0; i < fs_count; i++) {
        usize plen = str_len(dir_path);
        if (fs_nodes[i].is_dir) {
            /* check if this dir is a direct child */
            if (str_ncmp(fs_nodes[i].path, dir_path, plen) == 0 &&
                fs_nodes[i].path[plen] == '/' &&
                str_len(fs_nodes[i].path) > plen + 1) {
                /* check no further slashes */
                int has_more = 0;
                for (usize j = plen + 1; fs_nodes[i].path[j]; j++) {
                    if (fs_nodes[i].path[j] == '/') { has_more = 1; break; }
                }
                if (!has_more) {
                    out_write(fs_nodes[i].name);
                    out_write("/\n");
                    found = 1;
                }
            }
        } else {
            if (str_ncmp(fs_nodes[i].path, dir_path, plen) == 0 &&
                fs_nodes[i].path[plen] == '/' &&
                str_len(fs_nodes[i].path) > plen + 1) {
                int has_more = 0;
                for (usize j = plen + 1; fs_nodes[i].path[j]; j++) {
                    if (fs_nodes[i].path[j] == '/') { has_more = 1; break; }
                }
                if (!has_more) {
                    out_write(fs_nodes[i].name);
                    out_write("\n");
                    found = 1;
                }
            }
        }
    }
    if (!found) {
        out_write("(empty)\n");
    }
}

static void fs_list_dir_long(const char* dir_path) {
    int found = 0;
    for (int i = 0; i < fs_count; i++) {
        usize plen = str_len(dir_path);
        if (str_ncmp(fs_nodes[i].path, dir_path, plen) == 0 &&
            fs_nodes[i].path[plen] == '/' &&
            str_len(fs_nodes[i].path) > plen + 1) {
            int has_more = 0;
            for (usize j = plen + 1; fs_nodes[i].path[j]; j++) {
                if (fs_nodes[i].path[j] == '/') { has_more = 1; break; }
            }
            if (has_more) continue;

            if (fs_nodes[i].is_dir) {
                out_write("drwxr-xr-x  usr40k  usr40k  ");
                out_write("4096  ");
                out_write(fs_nodes[i].name);
                out_write("/\n");
            } else {
                out_write("-rw-r--r--  usr40k  usr40k  ");
                out_write_int((int)fs_nodes[i].content_len);
                out_write("  ");
                out_write(fs_nodes[i].name);
                out_write("\n");
            }
            found = 1;
        }
    }
    if (!found) {
        out_write("total 0\n");
    }
}

/* ------------------------------------------------------------------ */
/* Environment variables                                               */
/* ------------------------------------------------------------------ */

#define MAX_ENV 16
#define MAX_ENV_KEY 32
#define MAX_ENV_VAL 128

static char env_keys[MAX_ENV][MAX_ENV_KEY];
static char env_vals[MAX_ENV][MAX_ENV_VAL];
static int env_count = 0;

static void env_init(void) {
    env_count = 0;
    str_cpy(env_keys[env_count], "HOME");
    str_cpy(env_vals[env_count], "/home/usr40k");
    env_count++;
    str_cpy(env_keys[env_count], "USER");
    str_cpy(env_vals[env_count], "usr40k");
    env_count++;
    str_cpy(env_keys[env_count], "SHELL");
    str_cpy(env_vals[env_count], "/bin/dumsh");
    env_count++;
    str_cpy(env_keys[env_count], "TERM");
    str_cpy(env_vals[env_count], "xterm-256color");
    env_count++;
    str_cpy(env_keys[env_count], "PATH");
    str_cpy(env_vals[env_count], "/home/usr40k/.local/bin:/usr/local/bin:/usr/bin:/bin");
    env_count++;
    str_cpy(env_keys[env_count], "PWD");
    str_cpy(env_vals[env_count], cwd);
    env_count++;
}

static const char* env_get(const char* key) {
    for (int i = 0; i < env_count; i++) {
        if (str_cmp(env_keys[i], key) == 0) return env_vals[i];
    }
    return 0;
}

static void env_set(const char* key, const char* val) {
    for (int i = 0; i < env_count; i++) {
        if (str_cmp(env_keys[i], key) == 0) {
            str_cpy(env_vals[i], val);
            return;
        }
    }
    if (env_count < MAX_ENV) {
        str_cpy(env_keys[env_count], key);
        str_cpy(env_vals[env_count], val);
        env_count++;
    }
}

/* ------------------------------------------------------------------ */
/* Command history                                                     */
/* ------------------------------------------------------------------ */

#define MAX_HISTORY 50
#define MAX_HIST_CMD 256

static char history[MAX_HISTORY][MAX_HIST_CMD];
static int history_count = 0;

static void history_add(const char* cmd) {
    if (history_count < MAX_HISTORY) {
        str_cpy(history[history_count], cmd);
        history_count++;
    } else {
        /* shift */
        for (int i = 0; i < MAX_HISTORY - 1; i++) {
            str_cpy(history[i], history[i + 1]);
        }
        str_cpy(history[MAX_HISTORY - 1], cmd);
    }
}

/* ------------------------------------------------------------------ */
/* Command parsing                                                     */
/* ------------------------------------------------------------------ */

#define MAX_ARGS 16
#define MAX_ARG_LEN 128

typedef struct {
    char args[MAX_ARGS][MAX_ARG_LEN];
    int argc;
} ParsedCmd;

static void parse_cmd(const char* cmd, ParsedCmd* pc) {
    pc->argc = 0;
    int i = 0;
    int in_quote = 0;
    char cur[MAX_ARG_LEN];
    int cur_len = 0;

    while (cmd[i]) {
        char c = cmd[i];
        if (c == '"' || c == '\'') {
            in_quote = !in_quote;
            i++;
            continue;
        }
        if (is_space(c) && !in_quote) {
            if (cur_len > 0) {
                cur[cur_len] = 0;
                if (pc->argc < MAX_ARGS) {
                    str_cpy(pc->args[pc->argc], cur);
                    pc->argc++;
                }
                cur_len = 0;
            }
            i++;
            continue;
        }
        if (cur_len < MAX_ARG_LEN - 1) {
            cur[cur_len++] = c;
        }
        i++;
    }
    if (cur_len > 0) {
        cur[cur_len] = 0;
        if (pc->argc < MAX_ARGS) {
            str_cpy(pc->args[pc->argc], cur);
            pc->argc++;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Command implementations                                             */
/* ------------------------------------------------------------------ */

static void cmd_whoami(void) {
    out_write("usr40k\n");
}

static void cmd_pwd(void) {
    out_write(cwd);
    out_write("\n");
}

static void cmd_ls(ParsedCmd* pc) {
    int long_fmt = 0;
    int all = 0;
    int human = 0;
    char target[MAX_PATH];
    str_cpy(target, cwd);

    for (int i = 1; i < pc->argc; i++) {
        const char* arg = pc->args[i];
        /* handle combined flags like -la, -al, -lh, -lah */
        if (arg[0] == '-' && arg[1] && arg[1] != '-') {
            for (int j = 1; arg[j]; j++) {
                if (arg[j] == 'l') long_fmt = 1;
                else if (arg[j] == 'a') all = 1;
                else if (arg[j] == 'h') human = 1;
            }
        } else if (arg[0] != '-') {
            fs_resolve(arg, target);
        }
    }

    FSNode* dir = fs_find(target);
    if (!dir || !dir->is_dir) {
        out_write("ls: cannot access '");
        out_write(target);
        out_write("': No such file or directory\n");
        return;
    }

    if (long_fmt) {
        fs_list_dir_long(target);
    } else {
        fs_list_dir(target);
    }
}

static void cmd_cat(ParsedCmd* pc) {
    int number = 0;
    int start = 1;
    if (pc->argc > 1 && str_cmp(pc->args[1], "-n") == 0) {
        number = 1;
        start = 2;
    }
    if (pc->argc <= start) {
        out_write("cat: missing operand\n");
        return;
    }
    for (int i = start; i < pc->argc; i++) {
        char path[MAX_PATH];
        fs_resolve(pc->args[i], path);
        FSNode* f = fs_find(path);
        if (!f) {
            out_write("cat: ");
            out_write(pc->args[i]);
            out_write(": No such file or directory\n");
            continue;
        }
        if (f->is_dir) {
            out_write("cat: ");
            out_write(pc->args[i]);
            out_write(": Is a directory\n");
            continue;
        }
        if (number) {
            int line = 1;
            out_write_int(line);
            out_write("  ");
            for (usize j = 0; j < f->content_len; j++) {
                out_write_char(f->content[j]);
                if (f->content[j] == '\n' && j + 1 < f->content_len) {
                    line++;
                    out_write_int(line);
                    out_write("  ");
                }
            }
        } else {
            out_write(f->content);
        }
        if (f->content_len > 0 && f->content[f->content_len - 1] != '\n') {
            out_write("\n");
        }
    }
}

static void cmd_echo(ParsedCmd* pc) {
    int newline = 1;
    int escapes = 0;
    int start = 1;
    for (int i = 1; i < pc->argc; i++) {
        if (str_cmp(pc->args[i], "-n") == 0) newline = 0;
        else if (str_cmp(pc->args[i], "-e") == 0) escapes = 1;
        else if (str_cmp(pc->args[i], "-ne") == 0 || str_cmp(pc->args[i], "-en") == 0) {
            newline = 0;
            escapes = 1;
        } else {
            start = i;
            break;
        }
    }
    for (int i = start; i < pc->argc; i++) {
        if (i > start) out_write(" ");
        /* expand $VAR */
        if (pc->args[i][0] == '$' && pc->args[i][1]) {
            const char* val = env_get(&pc->args[i][1]);
            if (val) out_write(val);
        } else if (escapes) {
            /* handle escape sequences */
            const char* s = pc->args[i];
            for (int j = 0; s[j]; j++) {
                if (s[j] == '\\' && s[j + 1]) {
                    j++;
                    switch (s[j]) {
                        case 'n': out_write_char('\n'); break;
                        case 't': out_write_char('\t'); break;
                        case 'r': out_write_char('\r'); break;
                        case '\\': out_write_char('\\'); break;
                        case 'e': out_write_char('\x1b'); break;
                        default: out_write_char('\\'); out_write_char(s[j]); break;
                    }
                } else {
                    out_write_char(s[j]);
                }
            }
        } else {
            out_write(pc->args[i]);
        }
    }
    if (newline) out_write("\n");
}

static void cmd_cd(ParsedCmd* pc) {
    if (pc->argc < 2) {
        str_cpy(cwd, "/home/usr40k/workspace");
        env_set("PWD", cwd);
        return;
    }
    char path[MAX_PATH];
    fs_resolve(pc->args[1], path);
    FSNode* d = fs_find(path);
    if (!d || !d->is_dir) {
        out_write("cd: no such file or directory: ");
        out_write(pc->args[1]);
        out_write("\n");
        return;
    }
    str_cpy(cwd, path);
    env_set("PWD", cwd);
}

static void cmd_mkdir(ParsedCmd* pc) {
    int parents = 0;
    int start = 1;
    if (pc->argc > 1 && str_cmp(pc->args[1], "-p") == 0) {
        parents = 1;
        start = 2;
    }
    if (pc->argc <= start) {
        out_write("mkdir: missing operand\n");
        return;
    }
    for (int i = start; i < pc->argc; i++) {
        char path[MAX_PATH];
        fs_resolve(pc->args[i], path);
        if (fs_find(path)) {
            if (!parents) {
                out_write("mkdir: cannot create directory '");
                out_write(pc->args[i]);
                out_write("': File exists\n");
            }
            continue;
        }
        if (fs_count >= MAX_FILES) {
            out_write("mkdir: filesystem full\n");
            return;
        }
        FSNode* n = &fs_nodes[fs_count++];
        /* extract name from path */
        usize len = str_len(path);
        usize name_start = len;
        while (name_start > 0 && path[name_start - 1] != '/') name_start--;
        str_ncpy(n->name, &path[name_start], len - name_start);
        str_cpy(n->path, path);
        n->is_dir = 1;
        n->content_len = 0;
        n->perm = 0;
    }
}

static void cmd_touch(ParsedCmd* pc) {
    int no_create = 0;
    int start = 1;
    if (pc->argc > 1 && str_cmp(pc->args[1], "-c") == 0) {
        no_create = 1;
        start = 2;
    }
    if (pc->argc <= start) {
        out_write("touch: missing operand\n");
        return;
    }
    for (int i = start; i < pc->argc; i++) {
        char path[MAX_PATH];
        fs_resolve(pc->args[i], path);
        FSNode* f = fs_find(path);
        if (f) {
            /* update timestamp (no-op in this FS) */
            continue;
        }
        if (no_create) continue;
        if (fs_count >= MAX_FILES) {
            out_write("touch: filesystem full\n");
            return;
        }
        FSNode* n = &fs_nodes[fs_count++];
        usize len = str_len(path);
        usize name_start = len;
        while (name_start > 0 && path[name_start - 1] != '/') name_start--;
        str_ncpy(n->name, &path[name_start], len - name_start);
        str_cpy(n->path, path);
        n->is_dir = 0;
        n->content_len = 0;
        n->content[0] = 0;
        n->perm = 0;
    }
}

static void cmd_rm(ParsedCmd* pc) {
    int recursive = 0;
    int force = 0;
    int start = 1;
    for (int i = 1; i < pc->argc; i++) {
        if (str_cmp(pc->args[i], "-r") == 0 || str_cmp(pc->args[i], "-rf") == 0 || str_cmp(pc->args[i], "-fr") == 0) {
            recursive = 1;
            if (pc->args[i][1] == 'f' || pc->args[i][2] == 'f') force = 1;
        } else if (str_cmp(pc->args[i], "-f") == 0) {
            force = 1;
        } else {
            start = i;
            break;
        }
    }
    if (pc->argc <= start) {
        out_write("rm: missing operand\n");
        return;
    }
    for (int i = start; i < pc->argc; i++) {
        char path[MAX_PATH];
        fs_resolve(pc->args[i], path);
        FSNode* f = fs_find(path);
        if (!f) {
            if (!force) {
                out_write("rm: cannot remove '");
                out_write(pc->args[i]);
                out_write("': No such file or directory\n");
            }
            continue;
        }
        if (f->is_dir && !recursive) {
            out_write("rm: cannot remove '");
            out_write(pc->args[i]);
            out_write("': Is a directory\n");
            continue;
        }
        /* remove by shifting */
        for (int j = 0; j < fs_count; j++) {
            if (str_cmp(fs_nodes[j].path, path) == 0) {
                for (int k = j; k < fs_count - 1; k++) {
                    fs_nodes[k] = fs_nodes[k + 1];
                }
                fs_count--;
                break;
            }
        }
    }
}

static void cmd_grep(ParsedCmd* pc) {
    int ignore_case = 0;
    int line_nums = 0;
    int start = 1;
    for (int i = 1; i < pc->argc; i++) {
        if (str_cmp(pc->args[i], "-i") == 0) ignore_case = 1;
        else if (str_cmp(pc->args[i], "-n") == 0) line_nums = 1;
        else if (str_cmp(pc->args[i], "-in") == 0 || str_cmp(pc->args[i], "-ni") == 0) {
            ignore_case = 1;
            line_nums = 1;
        } else {
            start = i;
            break;
        }
    }
    if (pc->argc < start + 2) {
        out_write("grep: usage: grep [-i] [-n] PATTERN FILE\n");
        return;
    }
    const char* pattern = pc->args[start];
    for (int i = start + 1; i < pc->argc; i++) {
        char path[MAX_PATH];
        fs_resolve(pc->args[i], path);
        FSNode* f = fs_find(path);
        if (!f) {
            out_write("grep: ");
            out_write(pc->args[i]);
            out_write(": No such file or directory\n");
            continue;
        }
        /* simple line-by-line search */
        const char* content = f->content;
        usize clen = f->content_len;
        usize line_start = 0;
        int line_num = 1;
        for (usize j = 0; j <= clen; j++) {
            if (j == clen || content[j] == '\n') {
                usize line_len = j - line_start;
                /* check if pattern is in this line */
                for (usize k = 0; k + str_len(pattern) <= line_len; k++) {
                    int match = 1;
                    for (usize m = 0; m < str_len(pattern); m++) {
                        char a = content[line_start + k + m];
                        char b = pattern[m];
                        if (ignore_case) {
                            a = to_lower(a);
                            b = to_lower(b);
                        }
                        if (a != b) { match = 0; break; }
                    }
                    if (match) {
                        if (line_nums) {
                            out_write_int(line_num);
                            out_write(":");
                        }
                        for (usize m = line_start; m < j; m++) {
                            out_write_char(content[m]);
                        }
                        out_write("\n");
                        break;
                    }
                }
                line_start = j + 1;
                line_num++;
            }
        }
    }
}

static void cmd_wc(ParsedCmd* pc) {
    int show_lines = 0, show_words = 0, show_chars = 0;
    int start = 1;
    for (int i = 1; i < pc->argc; i++) {
        if (str_cmp(pc->args[i], "-l") == 0) show_lines = 1;
        else if (str_cmp(pc->args[i], "-w") == 0) show_words = 1;
        else if (str_cmp(pc->args[i], "-c") == 0) show_chars = 1;
        else if (str_cmp(pc->args[i], "-lw") == 0 || str_cmp(pc->args[i], "-wl") == 0) {
            show_lines = 1;
            show_words = 1;
        } else if (str_cmp(pc->args[i], "-lc") == 0 || str_cmp(pc->args[i], "-cl") == 0) {
            show_lines = 1;
            show_chars = 1;
        } else if (str_cmp(pc->args[i], "-wc") == 0 || str_cmp(pc->args[i], "-cw") == 0) {
            show_words = 1;
            show_chars = 1;
        } else if (str_cmp(pc->args[i], "-lwc") == 0 || str_cmp(pc->args[i], "-lcw") == 0 ||
                   str_cmp(pc->args[i], "-wlc") == 0 || str_cmp(pc->args[i], "-wcl") == 0 ||
                   str_cmp(pc->args[i], "-clw") == 0 || str_cmp(pc->args[i], "-cwl") == 0) {
            show_lines = 1;
            show_words = 1;
            show_chars = 1;
        } else {
            start = i;
            break;
        }
    }
    if (pc->argc <= start) {
        out_write("wc: missing operand\n");
        return;
    }
    /* default: show all */
    if (!show_lines && !show_words && !show_chars) {
        show_lines = show_words = show_chars = 1;
    }
    for (int i = start; i < pc->argc; i++) {
        char path[MAX_PATH];
        fs_resolve(pc->args[i], path);
        FSNode* f = fs_find(path);
        if (!f) {
            out_write("wc: ");
            out_write(pc->args[i]);
            out_write(": No such file or directory\n");
            continue;
        }
        int lines = 0, words = 0, chars = 0;
        int in_word = 0;
        for (usize j = 0; j < f->content_len; j++) {
            char c = f->content[j];
            chars++;
            if (c == '\n') lines++;
            if (is_space(c)) {
                in_word = 0;
            } else if (!in_word) {
                in_word = 1;
                words++;
            }
        }
        if (show_lines) { out_write_int(lines); out_write(" "); }
        if (show_words) { out_write_int(words); out_write(" "); }
        if (show_chars) { out_write_int(chars); out_write(" "); }
        out_write(f->name);
        out_write("\n");
    }
}

static void cmd_head(ParsedCmd* pc) {
    int lines = 10;
    int start = 1;
    if (pc->argc > 1) {
        if (pc->args[1][0] == '-' && is_digit(pc->args[1][1])) {
            lines = str_to_int(&pc->args[1][1]);
            start = 2;
        } else if (str_cmp(pc->args[1], "-n") == 0 && pc->argc > 2) {
            lines = str_to_int(pc->args[2]);
            start = 3;
        }
    }
    if (pc->argc <= start) {
        out_write("head: missing operand\n");
        return;
    }
    for (int i = start; i < pc->argc; i++) {
        char path[MAX_PATH];
        fs_resolve(pc->args[i], path);
        FSNode* f = fs_find(path);
        if (!f) {
            out_write("head: ");
            out_write(pc->args[i]);
            out_write(": No such file or directory\n");
            continue;
        }
        int count = 0;
        for (usize j = 0; j < f->content_len && count < lines; j++) {
            out_write_char(f->content[j]);
            if (f->content[j] == '\n') count++;
        }
        if (count < lines && f->content_len > 0 && f->content[f->content_len - 1] != '\n') {
            out_write("\n");
        }
    }
}

static void cmd_tail(ParsedCmd* pc) {
    int lines = 10;
    int start = 1;
    if (pc->argc > 1) {
        if (pc->args[1][0] == '-' && is_digit(pc->args[1][1])) {
            lines = str_to_int(&pc->args[1][1]);
            start = 2;
        } else if (str_cmp(pc->args[1], "-n") == 0 && pc->argc > 2) {
            lines = str_to_int(pc->args[2]);
            start = 3;
        }
    }
    if (pc->argc <= start) {
        out_write("tail: missing operand\n");
        return;
    }
    for (int i = start; i < pc->argc; i++) {
        char path[MAX_PATH];
        fs_resolve(pc->args[i], path);
        FSNode* f = fs_find(path);
        if (!f) {
            out_write("tail: ");
            out_write(pc->args[i]);
            out_write(": No such file or directory\n");
            continue;
        }
        /* count total lines */
        int total = 0;
        for (usize j = 0; j < f->content_len; j++) {
            if (f->content[j] == '\n') total++;
        }
        if (f->content_len > 0 && f->content[f->content_len - 1] != '\n') total++;

        int skip = total - lines;
        if (skip < 0) skip = 0;
        int line = 0;
        int started = 0;
        for (usize j = 0; j < f->content_len; j++) {
            if (line >= skip) {
                out_write_char(f->content[j]);
                started = 1;
            }
            if (f->content[j] == '\n') line++;
        }
        if (started && f->content_len > 0 && f->content[f->content_len - 1] != '\n') {
            out_write("\n");
        }
    }
}

static void cmd_date(ParsedCmd* pc) {
    int utc = 0;
    for (int i = 1; i < pc->argc; i++) {
        if (str_cmp(pc->args[i], "-u") == 0) utc = 1;
    }
    if (utc) {
        out_write("Sat Aug 23 01:45:00 UTC 2026\n");
    } else {
        out_write("Fri Aug 22 21:45:00 EDT 2026\n");
    }
}

static void cmd_uname(ParsedCmd* pc) {
    int all = 0, sys = 0, rel = 0, mach = 0;
    for (int i = 1; i < pc->argc; i++) {
        if (str_cmp(pc->args[i], "-a") == 0) all = 1;
        else if (str_cmp(pc->args[i], "-s") == 0) sys = 1;
        else if (str_cmp(pc->args[i], "-r") == 0) rel = 1;
        else if (str_cmp(pc->args[i], "-m") == 0) mach = 1;
    }
    if (all || (!sys && !rel && !mach)) {
        out_write("WebAssembly wasm32 dumsh 1.0.0 wasm32 GNU/Web\n");
    } else {
        if (sys) out_write("WebAssembly\n");
        if (rel) out_write("1.0.0\n");
        if (mach) out_write("wasm32\n");
    }
}

static void cmd_neofetch(void) {
    out_write("        .--.       usr40k@dumsh\n");
    out_write("       |o_o |      ----------\n");
    out_write("       |:_/ |      OS: WebAssembly wasm32\n");
    out_write("      //   \\ \\     Host: Browser\n");
    out_write("     (|     | )    Kernel: wasm32\n");
    out_write("    /'\\_   _/`\\   Uptime: 0 mins\n");
    out_write("    \\___)=(___/   Shell: dumsh\n");
    out_write("                  Resolution: 1920x1080\n");
    out_write("                  DE: Browser\n");
    out_write("                  WM: None\n");
    out_write("                  Terminal: dumsh\n");
    out_write("                  CPU: wasm32 @ 0 MHz\n");
    out_write("                  Memory: 1MiB / 1MiB\n");
}

static void cmd_banner(void) {
    out_write("  ____  _   _ __  __ ____  _   _ \n");
    out_write(" |  _ \\| | | |  \\/  / ___|| | | |\n");
    out_write(" | | | | | | | |\\/| \\___ \\| |_| |\n");
    out_write(" | |_| | |_| | |  | |___) |  _  |\n");
    out_write(" |____/ \\___/|_|  |_|____/|_| |_|\n");
    out_write("  a dumb little shell in WebAssembly\n");
}

static void cmd_help(void) {
    out_write("dumsh — a dumb little shell in WebAssembly\n");
    out_write("------------------------------------------\n");
    out_write("Available commands:\n");
    out_write("  whoami    - print current user\n");
    out_write("  pwd       - print working directory\n");
    out_write("  ls        - list directory contents\n");
    out_write("             flags: -l long, -a all, -h human, -la/-al combined\n");
    out_write("  cd        - change directory\n");
    out_write("  cat       - print file contents\n");
    out_write("             flags: -n number lines\n");
    out_write("  echo      - print text (echo $VAR for env vars)\n");
    out_write("             flags: -n no newline, -e escape sequences\n");
    out_write("  mkdir     - create directory\n");
    out_write("             flags: -p create parents\n");
    out_write("  touch     - create empty file\n");
    out_write("             flags: -c don't create if missing\n");
    out_write("  rm        - remove file\n");
    out_write("             flags: -r recursive, -f force\n");
    out_write("  grep      - search for pattern in file\n");
    out_write("             flags: -i ignore case, -n line numbers, -r recursive\n");
    out_write("  wc        - count lines, words, chars\n");
    out_write("             flags: -l lines, -w words, -c chars\n");
    out_write("  head      - print first 10 lines\n");
    out_write("             flags: -n N lines, -N shorthand\n");
    out_write("  tail      - print last 10 lines\n");
    out_write("             flags: -n N lines, -N shorthand\n");
    out_write("  date      - print current date\n");
    out_write("             flags: -u UTC\n");
    out_write("  uname     - print system info\n");
    out_write("             flags: -a all, -s system, -r release, -m machine\n");
    out_write("  neofetch  - system info display\n");
    out_write("  env       - print environment variables\n");
    out_write("  export    - set environment variable (export KEY=VALUE)\n");
    out_write("  history   - show command history\n");
    out_write("             flags: -c clear, -n N last N commands\n");
    out_write("  banner    - print the dumsh logo\n");
    out_write("  clear     - clear the terminal\n");
    out_write("  help      - show this help\n");
}

static void cmd_env(void) {
    for (int i = 0; i < env_count; i++) {
        out_write(env_keys[i]);
        out_write("=");
        out_write(env_vals[i]);
        out_write("\n");
    }
}

static void cmd_export(ParsedCmd* pc) {
    if (pc->argc < 2) {
        cmd_env();
        return;
    }
    /* parse KEY=VALUE */
    const char* arg = pc->args[1];
    char key[MAX_ENV_KEY];
    int i = 0;
    while (arg[i] && arg[i] != '=' && i < MAX_ENV_KEY - 1) {
        key[i] = arg[i];
        i++;
    }
    key[i] = 0;
    if (arg[i] == '=') {
        env_set(key, &arg[i + 1]);
    } else {
        /* just export existing var */
        const char* val = env_get(key);
        if (val) env_set(key, val);
    }
}

static void cmd_history(ParsedCmd* pc) {
    int clear = 0;
    int last_n = -1;
    for (int i = 1; i < pc->argc; i++) {
        if (str_cmp(pc->args[i], "-c") == 0) clear = 1;
        else if (str_cmp(pc->args[i], "-n") == 0 && i + 1 < pc->argc) {
            last_n = str_to_int(pc->args[i + 1]);
            i++;
        }
    }
    if (clear) {
        history_count = 0;
        return;
    }
    int start = 0;
    if (last_n > 0 && last_n < history_count) {
        start = history_count - last_n;
    }
    for (int i = start; i < history_count; i++) {
        out_write_int(i + 1);
        out_write("  ");
        out_write(history[i]);
        out_write("\n");
    }
}

/* ------------------------------------------------------------------ */
/* Main dispatcher                                                     */
/* ------------------------------------------------------------------ */

static void shell_exec_internal(const char* cmd) {
    ParsedCmd pc;
    parse_cmd(cmd, &pc);

    if (pc.argc == 0) return;

    const char* cmd_name = pc.args[0];

    if (str_cmp(cmd_name, "whoami") == 0) cmd_whoami();
    else if (str_cmp(cmd_name, "pwd") == 0) cmd_pwd();
    else if (str_cmp(cmd_name, "ls") == 0) cmd_ls(&pc);
    else if (str_cmp(cmd_name, "cat") == 0) cmd_cat(&pc);
    else if (str_cmp(cmd_name, "echo") == 0) cmd_echo(&pc);
    else if (str_cmp(cmd_name, "cd") == 0) cmd_cd(&pc);
    else if (str_cmp(cmd_name, "mkdir") == 0) cmd_mkdir(&pc);
    else if (str_cmp(cmd_name, "touch") == 0) cmd_touch(&pc);
    else if (str_cmp(cmd_name, "rm") == 0) cmd_rm(&pc);
    else if (str_cmp(cmd_name, "grep") == 0) cmd_grep(&pc);
    else if (str_cmp(cmd_name, "wc") == 0) cmd_wc(&pc);
    else if (str_cmp(cmd_name, "head") == 0) cmd_head(&pc);
    else if (str_cmp(cmd_name, "tail") == 0) cmd_tail(&pc);
    else if (str_cmp(cmd_name, "date") == 0) cmd_date(&pc);
    else if (str_cmp(cmd_name, "uname") == 0) cmd_uname(&pc);
    else if (str_cmp(cmd_name, "neofetch") == 0) cmd_neofetch();
    else if (str_cmp(cmd_name, "env") == 0) cmd_env();
    else if (str_cmp(cmd_name, "export") == 0) cmd_export(&pc);
    else if (str_cmp(cmd_name, "history") == 0) cmd_history(&pc);
    else if (str_cmp(cmd_name, "banner") == 0) cmd_banner();
    else if (str_cmp(cmd_name, "help") == 0) cmd_help();
    else if (str_cmp(cmd_name, "clear") == 0) {
        /* handled by JS */
        out_write("\x1b[2J\x1b[H");
    }
    else {
        out_write("dumsh: command not found: ");
        out_write(cmd_name);
        out_write("\n");
    }
}

/* ------------------------------------------------------------------ */
/* Exported API                                                        */
/* ------------------------------------------------------------------ */

void shell_init(void) {
    mem_init();
    fs_init();
    env_init();
    out_reset();
}

int shell_exec(const char* cmd, int cmd_len) {
    /* copy command to a null-terminated buffer */
    char* buf = (char*)shell_alloc(cmd_len + 1);
    if (!buf) return -1;
    for (int i = 0; i < cmd_len; i++) buf[i] = cmd[i];
    buf[cmd_len] = 0;

    out_reset();
    history_add(buf);
    shell_exec_internal(buf);

    shell_free(buf);
    return 0;
}

char* shell_get_output(void) {
    return out_buf;
}

int shell_get_output_len(void) {
    return (int)out_len;
}