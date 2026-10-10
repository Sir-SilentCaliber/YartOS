#pragma once
#include <stdbool.h>
bool lapic_present(void);
void lapic_enable_xapic(void);
void lapic_eoi(void);
