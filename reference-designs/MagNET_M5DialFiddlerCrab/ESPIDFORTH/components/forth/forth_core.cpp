/*
 * forth_core.cpp - Minimal Forth interpreter for ESP-IDF build validation
 *
 * This is a stub implementation providing basic stack operations and
 * arithmetic so the REPL infrastructure can be tested. It will be
 * replaced with the full ESP32forth v7.0.8.0 engine once the Arduino
 * dependencies are stripped out.
 *
 * The raw ESP32forth source is preserved in ESP32forth.ino alongside
 * this file for reference during the porting effort.
 *
 * Supported words:
 *   Numbers (decimal, hex with 0x prefix)
 *   Arithmetic: + - * / mod negate abs min max
 *   Stack:      dup drop swap over rot nip tuck 2dup 2drop 2swap
 *   Comparison:  = <> < > <= >= 0= 0< 0>
 *   Logic:      and or xor invert
 *   I/O:        . .s cr emit
 *   Memory:     here allot , c,
 *   Defining:   : ; variable constant
 *   Control:    if else then do loop +loop begin until again while repeat
 *   Misc:       words bye depth pick
 *   String:     ." s"
 *
 * Licensed under the Apache License, Version 2.0
 */

#include "forth_core.h"

#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <cctype>
#include <cstdint>
#include <cinttypes>

// ----- Configuration -----
#define MAX_STACK     256
#define MAX_RSTACK    256
#define MAX_WORD_LEN  64
#define MAX_WORDS     512
#define MAX_INPUT     256
#define MAX_DICT_CODE 4096

// ----- Types -----
typedef intptr_t cell_t;

typedef void (*word_fn_t)(void);

enum WordType {
    WT_PRIMITIVE,   // C++ function pointer
    WT_COLON,       // Compiled Forth word (index into code[])
    WT_VARIABLE,    // Points to a cell in dictionary
    WT_CONSTANT,    // Holds a constant value
};

struct DictEntry {
    char name[MAX_WORD_LEN];
    WordType type;
    bool immediate;
    union {
        word_fn_t fn;       // WT_PRIMITIVE
        int code_start;     // WT_COLON: index into code[]
        cell_t *var_ptr;    // WT_VARIABLE
        cell_t const_val;   // WT_CONSTANT
    };
};

// ----- Global State -----
static cell_t  dstack[MAX_STACK];
static int     dsp = -1;  // data stack pointer

static cell_t  rstack[MAX_RSTACK];
static int     rsp = -1;  // return stack pointer

static DictEntry dictionary[MAX_WORDS];
static int dict_count = 0;

static cell_t  code[MAX_DICT_CODE];  // compiled code space
static int     code_ptr = 0;

static uint8_t *heap_mem = nullptr;
static int      heap_total = 0;
static int      heap_used_bytes = 0;

static int  (*io_getchar)(void) = nullptr;
static void (*io_putchar)(int)  = nullptr;

static bool running = true;
static bool compiling = false;
static int  base = 10;

// Forward declarations
static void execute_word(int idx);
static int  find_word(const char *name);
static bool parse_number(const char *word, cell_t *val);
static void interpret_token(const char *token);

// ----- Stack Helpers -----
static inline void push(cell_t v) {
    if (dsp < MAX_STACK - 1) dstack[++dsp] = v;
}

static inline cell_t pop(void) {
    if (dsp >= 0) return dstack[dsp--];
    return 0;
}

static inline cell_t top(void) {
    if (dsp >= 0) return dstack[dsp];
    return 0;
}

static inline void rpush(cell_t v) {
    if (rsp < MAX_RSTACK - 1) rstack[++rsp] = v;
}

static inline cell_t rpop(void) {
    if (rsp >= 0) return rstack[rsp--];
    return 0;
}

// ----- I/O Helpers -----
static void put_string(const char *s) {
    if (!io_putchar) return;
    while (*s) io_putchar(*s++);
}

static void put_number(cell_t n) {
    char buf[32];
    if (base == 16) {
        snprintf(buf, sizeof(buf), "%" PRIxPTR, (uintptr_t)n);
    } else {
        snprintf(buf, sizeof(buf), "%" PRIdPTR, n);
    }
    put_string(buf);
}

// ----- Primitive Words -----
static void w_add(void)     { cell_t b = pop(); cell_t a = pop(); push(a + b); }
static void w_sub(void)     { cell_t b = pop(); cell_t a = pop(); push(a - b); }
static void w_mul(void)     { cell_t b = pop(); cell_t a = pop(); push(a * b); }
static void w_div(void)     { cell_t b = pop(); cell_t a = pop(); if (b) push(a / b); else { put_string("? division by zero\n"); push(0); } }
static void w_mod(void)     { cell_t b = pop(); cell_t a = pop(); if (b) push(a % b); else { put_string("? division by zero\n"); push(0); } }
static void w_negate(void)  { push(-pop()); }
static void w_abs(void)     { cell_t v = pop(); push(v < 0 ? -v : v); }
static void w_min(void)     { cell_t b = pop(); cell_t a = pop(); push(a < b ? a : b); }
static void w_max(void)     { cell_t b = pop(); cell_t a = pop(); push(a > b ? a : b); }

static void w_dup(void)     { push(top()); }
static void w_drop(void)    { pop(); }
static void w_swap(void)    { cell_t b = pop(); cell_t a = pop(); push(b); push(a); }
static void w_over(void)    { cell_t b = pop(); cell_t a = pop(); push(a); push(b); push(a); }
static void w_rot(void)     { cell_t c = pop(); cell_t b = pop(); cell_t a = pop(); push(b); push(c); push(a); }
static void w_nip(void)     { cell_t b = pop(); pop(); push(b); }
static void w_tuck(void)    { cell_t b = pop(); cell_t a = pop(); push(b); push(a); push(b); }
static void w_2dup(void)    { cell_t b = pop(); cell_t a = pop(); push(a); push(b); push(a); push(b); }
static void w_2drop(void)   { pop(); pop(); }
static void w_2swap(void)   { cell_t d = pop(); cell_t c = pop(); cell_t b = pop(); cell_t a = pop();
                               push(c); push(d); push(a); push(b); }
static void w_depth(void)   { push(dsp + 1); }
static void w_pick(void)    { cell_t n = pop(); if (dsp - n >= 0) push(dstack[dsp - n]); else push(0); }

static void w_eq(void)      { cell_t b = pop(); cell_t a = pop(); push(a == b ? -1 : 0); }
static void w_neq(void)     { cell_t b = pop(); cell_t a = pop(); push(a != b ? -1 : 0); }
static void w_lt(void)      { cell_t b = pop(); cell_t a = pop(); push(a < b ? -1 : 0); }
static void w_gt(void)      { cell_t b = pop(); cell_t a = pop(); push(a > b ? -1 : 0); }
static void w_le(void)      { cell_t b = pop(); cell_t a = pop(); push(a <= b ? -1 : 0); }
static void w_ge(void)      { cell_t b = pop(); cell_t a = pop(); push(a >= b ? -1 : 0); }
static void w_zeq(void)     { push(pop() == 0 ? -1 : 0); }
static void w_zlt(void)     { push(pop() < 0 ? -1 : 0); }
static void w_zgt(void)     { push(pop() > 0 ? -1 : 0); }

static void w_and(void)     { cell_t b = pop(); cell_t a = pop(); push(a & b); }
static void w_or(void)      { cell_t b = pop(); cell_t a = pop(); push(a | b); }
static void w_xor(void)     { cell_t b = pop(); cell_t a = pop(); push(a ^ b); }
static void w_invert(void)  { push(~pop()); }

static void w_dot(void)     { put_number(pop()); io_putchar(' '); }
static void w_cr(void)      { io_putchar('\n'); }
static void w_emit(void)    { io_putchar((int)pop()); }

static void w_dots(void) {
    put_string("<");
    put_number(dsp + 1);
    put_string("> ");
    for (int i = 0; i <= dsp; i++) {
        put_number(dstack[i]);
        io_putchar(' ');
    }
}

static void w_words(void) {
    int col = 0;
    for (int i = 0; i < dict_count; i++) {
        int len = strlen(dictionary[i].name);
        if (col + len + 1 > 72) {
            io_putchar('\n');
            col = 0;
        }
        put_string(dictionary[i].name);
        io_putchar(' ');
        col += len + 1;
    }
    io_putchar('\n');
}

static void w_bye(void) {
    running = false;
}

static void w_here(void) {
    push((cell_t)(heap_mem + heap_used_bytes));
}

static void w_allot(void) {
    cell_t n = pop();
    if (heap_used_bytes + n <= heap_total) {
        heap_used_bytes += n;
    } else {
        put_string("? heap overflow\n");
    }
}

static void w_hex(void) { base = 16; }
static void w_decimal(void) { base = 10; }

static void w_to_r(void)    { rpush(pop()); }
static void w_r_from(void)  { push(rpop()); }
static void w_r_at(void)    { if (rsp >= 0) push(rstack[rsp]); else push(0); }

static void w_store(void)   { cell_t *addr = (cell_t *)pop(); *addr = pop(); }
static void w_fetch(void)   { cell_t *addr = (cell_t *)pop(); push(*addr); }
static void w_cstore(void)  { uint8_t *addr = (uint8_t *)pop(); *addr = (uint8_t)pop(); }
static void w_cfetch(void)  { uint8_t *addr = (uint8_t *)pop(); push(*addr); }

// ----- Dictionary Helpers -----
static void add_primitive(const char *name, word_fn_t fn, bool imm = false) {
    if (dict_count >= MAX_WORDS) return;
    DictEntry &e = dictionary[dict_count++];
    strncpy(e.name, name, MAX_WORD_LEN - 1);
    e.name[MAX_WORD_LEN - 1] = '\0';
    e.type = WT_PRIMITIVE;
    e.immediate = imm;
    e.fn = fn;
}

static int find_word(const char *name) {
    // Search backwards (most recent definition first)
    for (int i = dict_count - 1; i >= 0; i--) {
        if (strcasecmp(dictionary[i].name, name) == 0) return i;
    }
    return -1;
}

// ----- Number Parsing -----
static bool parse_number(const char *word, cell_t *val) {
    char *end;
    int parse_base = base;

    // Handle 0x prefix for hex
    if (word[0] == '0' && (word[1] == 'x' || word[1] == 'X')) {
        parse_base = 16;
        word += 2;
    }
    // Handle $ prefix for hex
    if (word[0] == '$' && word[1] != '\0') {
        parse_base = 16;
        word += 1;
    }
    // Handle # prefix for decimal
    if (word[0] == '#' && word[1] != '\0') {
        parse_base = 10;
        word += 1;
    }
    // Handle % prefix for binary
    if (word[0] == '%' && word[1] != '\0') {
        parse_base = 2;
        word += 1;
    }

    long long result = strtoll(word, &end, parse_base);
    if (*end == '\0' && end != word) {
        *val = (cell_t)result;
        return true;
    }
    return false;
}

// ----- Compiled Code Execution -----
// Code array entries use tagged values:
//   Positive indices = word index in dictionary (execute it)
//   Special negative values = control flow markers

#define CODE_LIT      (-1)   // next cell is a literal value
#define CODE_BRANCH   (-2)   // unconditional branch, next cell is target
#define CODE_0BRANCH  (-3)   // branch if TOS is 0, next cell is target
#define CODE_EXIT     (-4)   // return from colon def
#define CODE_DO       (-5)   // ( limit index -- ) start DO loop
#define CODE_LOOP     (-6)   // loop increment, next cell is branch target
#define CODE_PLOOP    (-7)   // +loop increment, next cell is branch target
#define CODE_DOTQUOTE (-8)   // next cell is string length, then chars

static void execute_code(int start) {
    int ip = start;
    while (ip < code_ptr) {
        cell_t op = code[ip++];
        if (op == CODE_LIT) {
            push(code[ip++]);
        } else if (op == CODE_BRANCH) {
            ip = (int)code[ip];
        } else if (op == CODE_0BRANCH) {
            cell_t cond = pop();
            if (cond == 0) {
                ip = (int)code[ip];
            } else {
                ip++;
            }
        } else if (op == CODE_EXIT) {
            return;
        } else if (op == CODE_DO) {
            cell_t index = pop();
            cell_t limit = pop();
            rpush(limit);
            rpush(index);
        } else if (op == CODE_LOOP) {
            cell_t target = code[ip++];
            rstack[rsp] += 1;
            if (rstack[rsp] >= rstack[rsp - 1]) {
                rpop(); rpop();  // drop index and limit
            } else {
                ip = (int)target;
            }
        } else if (op == CODE_PLOOP) {
            cell_t target = code[ip++];
            cell_t inc = pop();
            cell_t old_index = rstack[rsp];
            rstack[rsp] += inc;
            cell_t new_index = rstack[rsp];
            cell_t limit = rstack[rsp - 1];
            // Check crossing: (old_index - limit) and (new_index - limit) have different signs
            bool crossed = ((old_index - limit) ^ (new_index - limit)) < 0;
            if (crossed) {
                rpop(); rpop();
            } else {
                ip = (int)target;
            }
        } else if (op == CODE_DOTQUOTE) {
            int len = (int)code[ip++];
            const char *str = (const char *)&code[ip];
            for (int i = 0; i < len; i++) io_putchar(str[i]);
            ip += (len + sizeof(cell_t) - 1) / sizeof(cell_t);
        } else if (op >= 0 && op < dict_count) {
            execute_word((int)op);
        }
    }
}

static void execute_word(int idx) {
    DictEntry &e = dictionary[idx];
    switch (e.type) {
        case WT_PRIMITIVE:
            e.fn();
            break;
        case WT_COLON:
            execute_code(e.code_start);
            break;
        case WT_VARIABLE:
            push((cell_t)e.var_ptr);
            break;
        case WT_CONSTANT:
            push(e.const_val);
            break;
    }
}

// ----- Tokenizer -----
static const char *input_line = nullptr;
static int input_pos = 0;
static int input_len = 0;

static bool next_token(char *buf, int bufsize) {
    // Skip whitespace
    while (input_pos < input_len && isspace((unsigned char)input_line[input_pos]))
        input_pos++;
    if (input_pos >= input_len) return false;

    int i = 0;
    while (input_pos < input_len && !isspace((unsigned char)input_line[input_pos]) && i < bufsize - 1) {
        buf[i++] = input_line[input_pos++];
    }
    buf[i] = '\0';
    return i > 0;
}

// Read until delimiter (for ." and s")
static int read_until(char delim, char *buf, int bufsize) {
    // Skip one leading space if present
    if (input_pos < input_len && input_line[input_pos] == ' ')
        input_pos++;

    int i = 0;
    while (input_pos < input_len && input_line[input_pos] != delim && i < bufsize - 1) {
        buf[i++] = input_line[input_pos++];
    }
    if (input_pos < input_len && input_line[input_pos] == delim)
        input_pos++;  // skip delimiter
    buf[i] = '\0';
    return i;
}

// ----- Compiler / Interpreter -----
static int compile_start = 0;

// Forward-reference patching stacks for control structures
#define MAX_CTRL_STACK 32
static int ctrl_stack[MAX_CTRL_STACK];
static int ctrl_sp = -1;

static void ctrl_push(int v) { if (ctrl_sp < MAX_CTRL_STACK - 1) ctrl_stack[++ctrl_sp] = v; }
static int  ctrl_pop(void) { return ctrl_sp >= 0 ? ctrl_stack[ctrl_sp--] : 0; }

static void interpret_token(const char *token) {
    cell_t val;

    // Handle colon definition start
    if (strcmp(token, ":") == 0) {
        char name[MAX_WORD_LEN];
        if (!next_token(name, sizeof(name))) {
            put_string("? missing name after :\n");
            return;
        }
        if (dict_count >= MAX_WORDS) {
            put_string("? dictionary full\n");
            return;
        }
        compiling = true;
        compile_start = code_ptr;
        DictEntry &e = dictionary[dict_count];
        strncpy(e.name, name, MAX_WORD_LEN - 1);
        e.name[MAX_WORD_LEN - 1] = '\0';
        e.type = WT_COLON;
        e.immediate = false;
        e.code_start = code_ptr;
        // Don't increment dict_count yet; do it at ;
        return;
    }

    if (strcmp(token, ";") == 0 && compiling) {
        code[code_ptr++] = CODE_EXIT;
        compiling = false;
        dict_count++;
        return;
    }

    // Handle variable
    if (strcasecmp(token, "variable") == 0) {
        char name[MAX_WORD_LEN];
        if (!next_token(name, sizeof(name))) {
            put_string("? missing name\n");
            return;
        }
        if (dict_count >= MAX_WORDS) return;
        // Allocate one cell from heap
        cell_t *ptr = (cell_t *)(heap_mem + heap_used_bytes);
        *ptr = 0;
        heap_used_bytes += sizeof(cell_t);
        DictEntry &e = dictionary[dict_count++];
        strncpy(e.name, name, MAX_WORD_LEN - 1);
        e.name[MAX_WORD_LEN - 1] = '\0';
        e.type = WT_VARIABLE;
        e.immediate = false;
        e.var_ptr = ptr;
        return;
    }

    // Handle constant
    if (strcasecmp(token, "constant") == 0) {
        char name[MAX_WORD_LEN];
        if (!next_token(name, sizeof(name))) {
            put_string("? missing name\n");
            return;
        }
        if (dict_count >= MAX_WORDS) return;
        cell_t val_c = pop();
        DictEntry &e = dictionary[dict_count++];
        strncpy(e.name, name, MAX_WORD_LEN - 1);
        e.name[MAX_WORD_LEN - 1] = '\0';
        e.type = WT_CONSTANT;
        e.immediate = false;
        e.const_val = val_c;
        return;
    }

    // Handle ." in compile mode
    if (strcmp(token, ".\"") == 0) {
        char str[256];
        int len = read_until('"', str, sizeof(str));
        if (compiling) {
            code[code_ptr++] = CODE_DOTQUOTE;
            code[code_ptr++] = len;
            // Pack string into code cells
            int cells_needed = (len + sizeof(cell_t) - 1) / sizeof(cell_t);
            memset(&code[code_ptr], 0, cells_needed * sizeof(cell_t));
            memcpy(&code[code_ptr], str, len);
            code_ptr += cells_needed;
        } else {
            put_string(str);
        }
        return;
    }

    // Handle ( comment
    if (strcmp(token, "(") == 0) {
        char dummy[256];
        read_until(')', dummy, sizeof(dummy));
        return;
    }

    // Handle \ line comment
    if (strcmp(token, "\\") == 0) {
        input_pos = input_len;  // skip rest of line
        return;
    }

    // Compile-mode control structures
    if (compiling) {
        if (strcasecmp(token, "if") == 0) {
            code[code_ptr++] = CODE_0BRANCH;
            ctrl_push(code_ptr);
            code[code_ptr++] = 0;  // placeholder
            return;
        }
        if (strcasecmp(token, "else") == 0) {
            code[code_ptr++] = CODE_BRANCH;
            int else_jump = code_ptr;
            code[code_ptr++] = 0;  // placeholder
            int if_addr = ctrl_pop();
            code[if_addr] = code_ptr;  // patch IF's branch
            ctrl_push(else_jump);
            return;
        }
        if (strcasecmp(token, "then") == 0) {
            int addr = ctrl_pop();
            code[addr] = code_ptr;  // patch branch target
            return;
        }
        if (strcasecmp(token, "begin") == 0) {
            ctrl_push(code_ptr);
            return;
        }
        if (strcasecmp(token, "until") == 0) {
            code[code_ptr++] = CODE_0BRANCH;
            code[code_ptr++] = ctrl_pop();
            return;
        }
        if (strcasecmp(token, "again") == 0) {
            code[code_ptr++] = CODE_BRANCH;
            code[code_ptr++] = ctrl_pop();
            return;
        }
        if (strcasecmp(token, "while") == 0) {
            code[code_ptr++] = CODE_0BRANCH;
            ctrl_push(code_ptr);
            code[code_ptr++] = 0;  // placeholder
            return;
        }
        if (strcasecmp(token, "repeat") == 0) {
            int while_addr = ctrl_pop();
            int begin_addr = ctrl_pop();
            code[code_ptr++] = CODE_BRANCH;
            code[code_ptr++] = begin_addr;
            code[while_addr] = code_ptr;  // patch WHILE's branch
            return;
        }
        if (strcasecmp(token, "do") == 0) {
            code[code_ptr++] = CODE_DO;
            ctrl_push(code_ptr);
            return;
        }
        if (strcasecmp(token, "loop") == 0) {
            code[code_ptr++] = CODE_LOOP;
            code[code_ptr++] = ctrl_pop();
            return;
        }
        if (strcasecmp(token, "+loop") == 0) {
            code[code_ptr++] = CODE_PLOOP;
            code[code_ptr++] = ctrl_pop();
            return;
        }
        // In compile mode: look up word or compile literal
        int idx = find_word(token);
        if (idx >= 0) {
            if (dictionary[idx].immediate) {
                execute_word(idx);
            } else {
                code[code_ptr++] = idx;
            }
            return;
        }
        if (parse_number(token, &val)) {
            code[code_ptr++] = CODE_LIT;
            code[code_ptr++] = val;
            return;
        }
        put_string("? compile: ");
        put_string(token);
        put_string("\n");
        compiling = false;  // abort compilation on error
        return;
    }

    // Interpret mode
    int idx = find_word(token);
    if (idx >= 0) {
        execute_word(idx);
        return;
    }

    if (parse_number(token, &val)) {
        push(val);
        return;
    }

    put_string("? ");
    put_string(token);
    put_string("\n");
}

static void interpret_line(const char *line) {
    input_line = line;
    input_pos = 0;
    input_len = strlen(line);

    char token[MAX_WORD_LEN];
    while (next_token(token, sizeof(token))) {
        interpret_token(token);
        if (!running) return;
    }
}

// DO loop index words
static void w_i(void) { if (rsp >= 0) push(rstack[rsp]); }
static void w_j(void) { if (rsp >= 2) push(rstack[rsp - 2]); }

// ----- Public API -----

extern "C" {

int forth_init(int heap_size_bytes) {
    heap_mem = (uint8_t *)malloc(heap_size_bytes);
    if (!heap_mem) return -1;
    heap_total = heap_size_bytes;
    heap_used_bytes = 0;
    memset(heap_mem, 0, heap_size_bytes);

    dsp = -1;
    rsp = -1;
    dict_count = 0;
    code_ptr = 0;
    compiling = false;
    running = true;
    base = 10;
    ctrl_sp = -1;

    // Register primitives
    add_primitive("+", w_add);
    add_primitive("-", w_sub);
    add_primitive("*", w_mul);
    add_primitive("/", w_div);
    add_primitive("mod", w_mod);
    add_primitive("negate", w_negate);
    add_primitive("abs", w_abs);
    add_primitive("min", w_min);
    add_primitive("max", w_max);

    add_primitive("dup", w_dup);
    add_primitive("drop", w_drop);
    add_primitive("swap", w_swap);
    add_primitive("over", w_over);
    add_primitive("rot", w_rot);
    add_primitive("nip", w_nip);
    add_primitive("tuck", w_tuck);
    add_primitive("2dup", w_2dup);
    add_primitive("2drop", w_2drop);
    add_primitive("2swap", w_2swap);
    add_primitive("depth", w_depth);
    add_primitive("pick", w_pick);

    add_primitive("=", w_eq);
    add_primitive("<>", w_neq);
    add_primitive("<", w_lt);
    add_primitive(">", w_gt);
    add_primitive("<=", w_le);
    add_primitive(">=", w_ge);
    add_primitive("0=", w_zeq);
    add_primitive("0<", w_zlt);
    add_primitive("0>", w_zgt);

    add_primitive("and", w_and);
    add_primitive("or", w_or);
    add_primitive("xor", w_xor);
    add_primitive("invert", w_invert);

    add_primitive(".", w_dot);
    add_primitive(".s", w_dots);
    add_primitive("cr", w_cr);
    add_primitive("emit", w_emit);
    add_primitive("words", w_words);
    add_primitive("bye", w_bye);

    add_primitive("here", w_here);
    add_primitive("allot", w_allot);
    add_primitive("hex", w_hex);
    add_primitive("decimal", w_decimal);

    add_primitive(">r", w_to_r);
    add_primitive("r>", w_r_from);
    add_primitive("r@", w_r_at);
    add_primitive("i", w_i);
    add_primitive("j", w_j);

    add_primitive("!", w_store);
    add_primitive("@", w_fetch);
    add_primitive("c!", w_cstore);
    add_primitive("c@", w_cfetch);

    return 0;
}

void forth_repl(int (*get_char)(void), void (*put_char)(int)) {
    io_getchar = get_char;
    io_putchar = put_char;

    put_string("ESP-IDF Forth (stub v0.1) - Type 'words' for vocabulary, 'bye' to exit\n");

    char line[MAX_INPUT];
    int pos = 0;
    running = true;

    put_string(compiling ? "] " : "ok> ");

    while (running) {
        int ch = io_getchar();
        if (ch < 0) {
            // No input available (timeout or error) - yield briefly
            continue;
        }

        if (ch == '\n') {
            continue;  /* Ignore \n, handle \r only (terminals send \r\n) */
        }
        if (ch == '\r') {
            io_putchar('\r');
            io_putchar('\n');
            line[pos] = '\0';
            if (pos > 0) {
                interpret_line(line);
                io_putchar('\r');
                io_putchar('\n');
            }
            pos = 0;
            if (running) {
                put_string("ok> ");
            }
        } else if (ch == 8 || ch == 127) {
            // Backspace
            if (pos > 0) {
                pos--;
                put_string("\b \b");
            }
        } else if (ch >= 32 && pos < MAX_INPUT - 1) {
            line[pos++] = (char)ch;
            io_putchar(ch);  // echo
        }
    }

    put_string("Bye!\n");
}

int forth_eval(const char *text) {
    if (!heap_mem) return -1;
    interpret_line(text);
    return 0;
}

int forth_heap_used(void) {
    return heap_used_bytes;
}

int forth_heap_free(void) {
    return heap_total - heap_used_bytes;
}

void forth_deinit(void) {
    if (heap_mem) {
        free(heap_mem);
        heap_mem = nullptr;
    }
    heap_total = 0;
    heap_used_bytes = 0;
    dsp = -1;
    rsp = -1;
    dict_count = 0;
    code_ptr = 0;
    running = false;
}

} // extern "C"
