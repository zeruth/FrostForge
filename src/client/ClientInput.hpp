#ifndef CLIENT_CLIENT_INPUT_HPP
#define CLIENT_CLIENT_INPUT_HPP

// The modifier keys held now (client/ClientLoop.cpp; the held-key mask DAT_142de0e24: 1/2
// shift, 4/8 control, 0x10/0x20 alt).

// ref: FUN_140236bb0
bool ClientIsShiftKeyDown();
// ref: FUN_140236b30
bool ClientIsControlKeyDown();
// ref: FUN_140236b00
bool ClientIsAltKeyDown();

#endif
