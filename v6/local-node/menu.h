#ifndef MENU_H
#define MENU_H
#include <Arduino.h>
#include "events.h"

void menu_init();
bool menu_isOpen();
bool menu_isEditing();          // true when value editor is active
void menu_handleButton(ButtonId id, PressKind k);
uint8_t menu_itemCount();
const char* menu_itemLabel(uint8_t idx);
uint8_t menu_selectedIndex();

// For UI rendering of the editor screen
const char* menu_editTitle();   // e.g. "MIN Water Level"
int32_t     menu_editValue();   // current value being edited
const char* menu_editUnit();
bool        menu_editIsDecimal();    // e.g. "%", "ms", "mV"

#endif
