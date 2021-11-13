#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>

#include "win.h"


#define PC_START 0x3000

#define FALSE 0
#define TRUE 1

#define DEBUG FALSE

typedef int bool;

enum Registers // Registers
{
    R_R0 = 0,
    R_R1 = 1,
    R_R2 = 2,
    R_R3 = 3,
    R_R4 = 4,
    R_R5 = 5,
    R_R6 = 6,
    R_R7 = 7,
    R_PC = 8, /* program counter */
    R_COND = 9,
    R_COUNT = 10
};

enum Opcodes // Opcodes
{
    OP_BR = 0,      /* branch */
    OP_ADD = 1,     /* add  */
    OP_LD = 2,      /* load */
    OP_ST = 3,      /* store */
    OP_JSR = 4,     /* jump register */
    OP_AND = 5,     /* bitwise and */
    OP_LDR = 6,     /* load register */
    OP_STR = 7,     /* store register */
    OP_RTI = 8,     /* unused */
    OP_NOT = 9,     /* bitwise not */
    OP_LDI = 10,    /* load indirect */
    OP_STI = 11,    /* store indirect */
    OP_JMP = 12,    /* jump */
    OP_RES = 13,    /* reserved (unused) */
    OP_LEA = 14,    /* load effective address */
    OP_TRAP = 15    /* execute trap */
};

enum ConditionFlags // Condition flags
{
    FL_POS = 1 << 0, /* P */
    FL_ZRO = 1 << 1, /* Z */
    FL_NEG = 1 << 2, /* N */
};

enum TrapCodes // Trap codes
{
    TRAP_GETC = 0x20,  /* get character from keyboard, not echoed onto the terminal */
    TRAP_OUT = 0x21,   /* output a character */
    TRAP_PUTS = 0x22,  /* output a word string */
    TRAP_IN = 0x23,    /* get character from keyboard, echoed onto the terminal */
    TRAP_PUTSP = 0x24, /* output a byte string */
    TRAP_HALT = 0x25   /* halt the program */
};

enum MemoryRegisters // Memory Mapped Registers
{
    MR_KBSR = 0xFE00, /* keyboard status */
    MR_KBDR = 0xFE02  /* keyboard data */
};


uint16_t memory[UINT16_MAX];
uint16_t reg[R_COUNT];

int running = TRUE;


char* binary_to_str(uint16_t value, int bit_length) {
    char* str = (char*) calloc(bit_length + 1, sizeof(char));
    str[bit_length + 1] = '\0';

    for (int i = 0; i < bit_length; i++)
        str[i] = ((value >> ((bit_length - 1) - i)) & 1) + '0';

    return str;
}

uint16_t swap16(uint16_t x)
{
    return (x << 8) | (x >> 8);
}
void read_image_file(FILE* file)
{
    /* the origin tells us where in memory to place the image */
    uint16_t origin;
    fread(&origin, sizeof(origin), 1, file);
    origin = swap16(origin);

    /* we know the maximum file size so we only need one fread */
    uint16_t max_read = UINT16_MAX - origin;
    uint16_t* p = memory + origin;
    size_t read = fread(p, sizeof(uint16_t), max_read, file);

    /* swap to little endian */
    while (read-- > 0)
    {
        *p = swap16(*p);
        ++p;
    }
}
int read_image(const char* image_path)
{
    FILE* file = fopen(image_path, "rb");
    if (!file) { return 0; };
    read_image_file(file);
    fclose(file);
    return 1;
}

void dump_memory(uint16_t* data, size_t size)
{
    FILE* dump_file = fopen("dump.txt", "w+");

    bool zero_block = FALSE;
    for (int i = 0; i < size; i++)
    {
        if (zero_block == FALSE && data[i] == 0)
        {
            fprintf(dump_file, "#%X: %d\n", i, data[i]);
            fprintf(dump_file, "...\n");
            zero_block = TRUE;
        }
        else if (zero_block == TRUE && data[i] == 0)
        {
            // do nothing
        }
        else
        {
            if (zero_block == TRUE)
            {
                // fprintf(dump_file, "#%X: %d\n", i - 1, swap16(data[i - 1]));
                zero_block = FALSE;
            }

            fprintf(dump_file, "#%X: %s opcode: %s\n", i,
                    binary_to_str(data[i], 16),
                    binary_to_str(data[i] >> 12, 4));
        }
    }

    fclose(dump_file);
}

uint16_t mem_read(uint16_t address)
{
    if (address == MR_KBSR)
    {
        if (check_key())
        {
            memory[MR_KBSR] = (1 << 15);
            memory[MR_KBDR] = getchar();
        }
        else
        {
            memory[MR_KBSR] = 0;
        }
    }

    return memory[address];
}

void mem_write(uint16_t address, uint16_t val)
{
    memory[address] = val;
}

uint16_t sign_extend(uint16_t x, int bit_count)
{
    if ((x >> (bit_count - 1)) & 1)
        x |= (0xFFFF << bit_count);
    
    return x;
}

uint16_t zero_extend(uint16_t x, int bit_count)
{
    x |= 0x0000 << bit_count;
    return x;
}

void update_flags(uint16_t r)
{
    if (reg[r] == 0)
        reg[R_COND] = FL_ZRO;
    else if (reg[r] >> 15) /* a 1 in the left-most bit indicates negative */
        reg[R_COND] = FL_NEG;
    else
        reg[R_COND] = FL_POS;
}


// Read a single character to R0 from the keyboard without echoing.
void trap_getc()
{
    reg[R_R0] = (uint16_t) getchar();
}

// Writes a character in R0[7:0] to the console display.
void trap_out()
{
    uint16_t character = reg[R_R0] & 0b11111111;
    putc((char) character, stdout);
    fflush(stdout);
}

// Write a string of ASCII characters starting in R0 to the console display.
void trap_puts()
{
    uint16_t* character = memory + reg[R_R0];
    while (*character != 0)
    {
        putc((char) *character, stdout);
        character++;
    }

    fflush(stdout);
}

// Print a prompt on the screen and read a single character from the keyboard into R0.
void trap_in()
{
    printf("Enter a character: ");
    char caracter = getchar();
    putc(caracter, stdout);
    
    reg[R_R0] = (uint16_t) caracter;
}

// Write a string of ASCII characters starting in R0 to the console display. Two characters per memory space.
void trap_putsp()
{
    uint16_t* characters = memory + reg[R_R0];
    while (*characters)
    {
        char char1 = (*characters) & 0xFF;
        putc(char1, stdout);
        
        char char2 = (*characters) >> 8;
        if (char2 != 0)
            putc(char2, stdout);

        characters++;
    }

    fflush(stdout);
}

// Halt execution and print a message on the console.
void trap_halt()
{
    puts("HALT");
    fflush(stdout);
    running = FALSE;
}


// Addition
void op_add(uint16_t instruction)
{
    if (DEBUG == TRUE) printf("op_add\n");

    /* destination register (DR) */
    uint16_t r0 = (instruction >> 9) & 0b111;
    /* first operand (SR1) */
    uint16_t r1 = (instruction >> 6) & 0b111;
    /* whether we are in immediate mode */
    uint16_t imm_flag = (instruction >> 5) & 0x1;

    if (imm_flag)
    {
        uint16_t imm5 = sign_extend(instruction & 0b11111, 5);
        reg[r0] = reg[r1] + imm5;
    }
    else
    {
        uint16_t r2 = instruction & 0x7;
        reg[r0] = reg[r1] + reg[r2];
    }

    update_flags(r0);
}

// Bit-wise Logical AND
void op_and(uint16_t instruction)
{
    if (DEBUG == TRUE) printf("op_and\n");

    uint16_t dr = (instruction >> 9) & 0b111;
    uint16_t sdr1 = (instruction >> 6) & 0b111;

    uint16_t immediate = (instruction >> 5) & 0b1;

    if (immediate == 0b0)
        reg[dr] = reg[sdr1] & reg[instruction & 0b111];
    else
        reg[dr] = reg[sdr1] & sign_extend(instruction & 0b11111, 5);

    update_flags(dr);
}

// Conditional Branch
void op_br(uint16_t instruction)
{
    if (DEBUG == TRUE) printf("op_br\n");

    // uint16_t n = (instruction << 11) & 0b1;
    // uint16_t z = (instruction << 10) & 0b1;
    // uint16_t p = (instruction << 9) & 0b1;

    uint16_t flags = (instruction >> 9) & 0b111;
    uint16_t offset = sign_extend(instruction & 0b111111111, 9);
    if ((flags & reg[R_COND]) > 0)
        reg[R_PC] += offset;
}

// Jump / Return from Subroutine
void op_jmp(uint16_t instruction)
{
    if (DEBUG == TRUE) printf("op_jmp %s", instruction == 0b1100000111000000 ? "ret\n" : "\n");

    uint16_t base_reg = (instruction >> 6) & 0b111;
    reg[R_PC] = reg[base_reg];
}

// Jump to Subroutine
void op_jsr(uint16_t instruction)
{
    if (DEBUG == TRUE) printf("op_jsr\n");

    uint16_t flag = (instruction >> 11) & 0b1;

    reg[R_R7] = reg[R_PC];
    if (flag == 0b1)
    {
        uint16_t offset = instruction & 0b11111111111;
        reg[R_PC] = reg[R_PC] + sign_extend(offset, 11);
    }
    else
    {
        uint16_t base_reg = (instruction >> 6) & 0b111;
        reg[R_PC] = reg[base_reg];
    }
}

// Load
void op_ld(uint16_t instruction)
{
    uint16_t dest_reg = (instruction >> 9) & 0b111;
    uint16_t offset = sign_extend(instruction & 0b111111111, 9);

    if (DEBUG == TRUE) printf("op_ld  %s %s | memory[PC(%s) + %s = %s] = %s",
        binary_to_str(dest_reg, 3),
        binary_to_str(offset, 9),
        binary_to_str(reg[R_PC], 16),
        binary_to_str(offset, 16),
        binary_to_str(reg[R_PC] + offset, 16),
        binary_to_str(mem_read(reg[R_PC] + offset), 16));

    reg[dest_reg] = mem_read(reg[R_PC] + offset);
    update_flags(dest_reg);

    if (DEBUG == TRUE) printf(" => DR(%s) \n", binary_to_str(reg[dest_reg], 16));
}

// Load Indirect
void op_ldi(uint16_t instruction)
{
    if (DEBUG == TRUE) printf("op_ldi\n");

    uint16_t dest_reg = (instruction >> 9) & 0b111;
    uint16_t offset = sign_extend(instruction & 0b111111111, 9);
    
    reg[dest_reg] = mem_read(mem_read(reg[R_PC] + offset));
    update_flags(dest_reg);
}

// Load Base + offset
void op_ldr(uint16_t instruction)
{
    if (DEBUG == TRUE) printf("op_ldr\n");

    uint16_t base_reg = (instruction >> 6) & 0b111;
    uint16_t dest_reg = (instruction >> 9) & 0b111;
    uint16_t offset = sign_extend(instruction & 0b111111, 6);

    reg[dest_reg] = mem_read(reg[base_reg] + offset);
    update_flags(dest_reg);
}

// Load Effective Address
void op_lea(uint16_t instruction)
{
    uint16_t dest_reg = (instruction >> 9) & 0b111;
    uint16_t offset = sign_extend(instruction & 0b111111111, 9);

    if (DEBUG == TRUE) printf("op_lea\n");
    // printf(" %s %s | PC(%s) + %s",
    //         binary_to_str(dest_reg, 3),
    //         binary_to_str(offset, 9),
    //         binary_to_str(reg[R_PC], 16),
    //         binary_to_str(offset, 16));

    reg[dest_reg] = reg[R_PC] + offset;
    update_flags(dest_reg);

    // printf(" => DR(%s) \n", binary_to_str(reg[dest_reg], 16));
}

// Bit-Wise Complement
void op_not(uint16_t instruction)
{
    if (DEBUG == TRUE) printf("op_not\n");

    uint16_t source_reg = (instruction >> 6) & 0b111;
    uint16_t dest_reg = (instruction >> 9) & 0b111;

    reg[dest_reg] = ~reg[source_reg];
    update_flags(dest_reg);
}

// Return from Interrupt
void op_rti(uint16_t instruction)
{

}

// Store
void op_st(uint16_t instruction)
{
    if (DEBUG == TRUE) printf("op_st\n");

    uint16_t offset = instruction & 0b111111111;
    uint16_t source_reg = (instruction >> 9) & 0b111;

    mem_write(reg[R_PC] + sign_extend(offset, 9), reg[source_reg]);
}

// Store indirect
void op_sti(uint16_t instruction)
{
    if (DEBUG == TRUE) printf("op_sti\n");

    uint16_t offset = instruction & 0b111111111;
    uint16_t source_reg = (instruction >> 9) & 0b111;
    
    mem_write(mem_read(reg[R_PC] + sign_extend(offset, 9)), reg[source_reg]);
}

// Store Base + offset
void op_str(uint16_t instruction)
{
    if (DEBUG == TRUE) printf("op_str: %s\n", binary_to_str(instruction, 16));

    uint16_t offset = sign_extend(instruction & 0b111111, 6);
    uint16_t base_reg = (instruction >> 6) & 0b111;
    uint16_t dest_reg = (instruction >> 9) & 0b111;

    mem_write(reg[base_reg] + offset, reg[dest_reg]);
}

// System Call
void op_trap(uint16_t instruction)
{
    if (DEBUG == TRUE) printf("op_trap\n");

    uint16_t vector = instruction & 0b11111111;

    reg[R_R7] = reg[R_PC];
    reg[R_PC] = mem_read(zero_extend(vector, 8));

    // Abstraindo chamada de TRAPs
    switch (instruction & 0xFF)
    {
    case TRAP_GETC:
        trap_getc();
        break;
    case TRAP_OUT:
        trap_out();
        break;
    case TRAP_PUTS:
        trap_puts();
        break;
    case TRAP_IN:
        trap_in();
        break;
    case TRAP_PUTSP:
        trap_putsp();
        break;
    case TRAP_HALT:
        trap_halt();
        break;
    }

    op_jmp(0b1100000111000000); // RET
}


int main(int argc, char** argv)
{
    signal(SIGINT, handle_interrupt);
    disable_input_buffering();

    //<editor-fold desc="Setup">

    if (argc < 2)
    {
        /* show usage string */
        printf("lc3 [image-file1] ...\n");
        exit(2);
    }

    for (int j = 1; j < argc; ++j)
    {
        if (!read_image(argv[j]))
        {
            printf("failed to load image: %s\n", argv[j]);
            exit(1);
        }
    }

    // dump_memory(memory, UINT16_MAX);

    //</editor-fold>


    reg[R_PC] = PC_START;

    while (running == TRUE)
    {
        uint16_t instruction = mem_read(reg[R_PC]++);
        uint16_t op = instruction >> 12;

        // printf("%s ", binary_to_str(instruction, 16));
        if (DEBUG == TRUE)
        {
            printf("#%X: %s opcode: %s\t", reg[R_PC] - 1,
                        binary_to_str(instruction, 16),
                        binary_to_str(op, 4));
        }

        switch (op)
        {
        case OP_BR:
            op_br(instruction);
            break;
        case OP_ADD:
            op_add(instruction);
            break;
        case OP_LD:
            op_ld(instruction);
            break;
        case OP_ST:
            op_st(instruction);
            break;
        case OP_JSR:
            op_jsr(instruction);
            break;
        case OP_AND:
            op_and(instruction);
            break;
        case OP_LDR:
            op_ldr(instruction);
            break;
        case OP_STR:
            op_str(instruction);
            break;
        case OP_NOT:
            op_not(instruction);
            break;
        case OP_LDI:
            op_ldi(instruction);
            break;
        case OP_STI:
            op_sti(instruction);
            break;
        case OP_JMP:
            op_jmp(instruction);
            break;
        case OP_LEA:
            op_lea(instruction);
            break;
        case OP_TRAP:
            op_trap(instruction);
            break;

        case OP_RES:
        case OP_RTI:
        default:
            restore_input_buffering();

            printf("Bad opcode %s", binary_to_str(instruction, 16));
            return EXIT_FAILURE;
        }
    }

    restore_input_buffering();

    return EXIT_SUCCESS;
}
