#ifndef AML_VALUES_H
#define AML_VALUES_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t id;
    const char *name;
} AmlTone;

static const AmlTone AML_TONES[] = {
    {0, "Off"},
    {1, "Buzzer"},
    {2, "Radio"},
    {3, "Melody 1"},
    {4, "Birds"},
    {5, "Bells"},
    {6, "Piano"},
    {7, "Melody 2"}
};

#define AML_TONE_COUNT (sizeof(AML_TONES) / sizeof(AML_TONES[0]))

#endif