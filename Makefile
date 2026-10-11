.RECIPEPREFIX := >

CC = gcc

CFLAGS = -Wall -Wextra -Wpedantic -std=c11 -O2
CPPFLAGS = -Isrc $(shell pkg-config --cflags json-c)
LDLIBS = -lbluetooth
JSON_C_LIBS = $(shell pkg-config --libs json-c)

OBJDIR = bin/obj

PROTOCOL_OBJ = $(OBJDIR)/protocol.o
STATE_OBJ = $(OBJDIR)/state.o
AML_OBJ = $(OBJDIR)/aml.o
CONFIG_OBJ = $(OBJDIR)/config.o
COMMAND_OBJ = $(OBJDIR)/command.o
REGISTRY_OBJ = $(OBJDIR)/registry.o
AML_COMMANDS_OBJ = $(OBJDIR)/aml_commands.o
SWEETDAWN_OBJ = $(OBJDIR)/sweetdawn.o
MAIN_OBJ = $(OBJDIR)/main.o

.PHONY: all test test-protocol test-state test-config \
        test-command test-aml test-aml-commands clean rebuild

# Compiler les modules et préparer les tests ainsi que le programme.
all: $(PROTOCOL_OBJ) $(STATE_OBJ) $(AML_OBJ) $(CONFIG_OBJ) \
     $(COMMAND_OBJ) $(REGISTRY_OBJ) $(AML_COMMANDS_OBJ) \
     $(SWEETDAWN_OBJ) bin/test_protocol bin/test_state \
     bin/test_config bin/test_command bin/test_aml \
     bin/test_aml_commands bin/sweetdawn

# Créer le répertoire des fichiers objets.
$(OBJDIR):
>mkdir -p $(OBJDIR)

# ============================================================
# Modules de base
# ============================================================

$(PROTOCOL_OBJ): src/protocol.c src/protocol.h | $(OBJDIR)
>$(CC) $(CPPFLAGS) $(CFLAGS) -c src/protocol.c -o $@

$(STATE_OBJ): src/state.c src/state.h | $(OBJDIR)
>$(CC) $(CPPFLAGS) $(CFLAGS) -c src/state.c -o $@

$(CONFIG_OBJ): src/config.c src/config.h | $(OBJDIR)
>$(CC) $(CPPFLAGS) $(CFLAGS) -c src/config.c -o $@

$(AML_OBJ): src/aml.c src/aml.h src/protocol.h src/config.h | $(OBJDIR)
>$(CC) $(CPPFLAGS) $(CFLAGS) -c src/aml.c -o $@

# ============================================================
# Arbre de commandes
# ============================================================

$(COMMAND_OBJ): src/command/command.c src/command/command.h | $(OBJDIR)
>$(CC) $(CPPFLAGS) $(CFLAGS) -c src/command/command.c -o $@

$(REGISTRY_OBJ): src/command/registry.c src/command/registry.h src/command/command.h | $(OBJDIR)
>$(CC) $(CPPFLAGS) $(CFLAGS) -c src/command/registry.c -o $@

$(AML_COMMANDS_OBJ): src/command/aml_commands.c src/command/aml_commands.h \
                     src/command/registry.h src/command/command.h \
                     $(COMMAND_OBJ) $(REGISTRY_OBJ) | $(OBJDIR)
>$(CC) $(CPPFLAGS) $(CFLAGS) -c src/command/aml_commands.c -o $@

# ============================================================
# Cœur applicatif SweetDawn
# ============================================================

$(SWEETDAWN_OBJ): src/sweetdawn.c src/sweetdawn.h \
                  src/aml.h src/config.h src/protocol.h src/state.h \
                  src/command/aml_commands.h src/command/registry.h \
                  | $(OBJDIR)
>$(CC) $(CPPFLAGS) $(CFLAGS) -c src/sweetdawn.c -o $@

$(MAIN_OBJ): src/main.c src/sweetdawn.h | $(OBJDIR)
>$(CC) $(CPPFLAGS) $(CFLAGS) -c src/main.c -o $@

# ============================================================
# Programme principal
# ============================================================

bin/sweetdawn: $(MAIN_OBJ) $(SWEETDAWN_OBJ) \
               $(PROTOCOL_OBJ) $(STATE_OBJ) $(AML_OBJ) $(CONFIG_OBJ) \
               $(COMMAND_OBJ) $(REGISTRY_OBJ) $(AML_COMMANDS_OBJ)
>mkdir -p bin
>$(CC) $(CFLAGS) $^ $(JSON_C_LIBS) $(LDLIBS) -o $@

# ============================================================
# Tests
# ============================================================

bin/test_protocol: test/test_protocol.c $(PROTOCOL_OBJ)
>mkdir -p bin
>$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@

bin/test_state: test/test_state.c $(STATE_OBJ)
>mkdir -p bin
>$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@

bin/test_config: test/test_config.c $(CONFIG_OBJ)
>mkdir -p bin
>$(CC) $(CPPFLAGS) $(CFLAGS) $^ $(JSON_C_LIBS) -o $@

bin/test_command: test/test_command.c \
                  src/command/command.c src/command/registry.c
>mkdir -p bin
>$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@

bin/test_aml: test/test_aml.c $(PROTOCOL_OBJ) $(AML_OBJ) $(CONFIG_OBJ)
>mkdir -p bin
>$(CC) $(CPPFLAGS) $(CFLAGS) $^ $(JSON_C_LIBS) $(LDLIBS) -o $@

bin/test_aml_commands: test/test_aml_commands.c \
                       src/command/aml_commands.c \
                       src/command/command.c src/command/registry.c
>mkdir -p bin
>$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@

# ============================================================
# Exécution des tests
# ============================================================

test: test-protocol test-state test-config test-command test-aml-commands

test-protocol: bin/test_protocol
>./bin/test_protocol

test-state: bin/test_state
>./bin/test_state

test-config: bin/test_config
>./bin/test_config

test-command: bin/test_command
>./bin/test_command

# Test d'intégration Bluetooth réel : à lancer volontairement.
test-aml: bin/test_aml
>./bin/test_aml

test-aml-commands: bin/test_aml_commands
>./bin/test_aml_commands

# ============================================================
# Nettoyage
# ============================================================

clean:
>rm -rf $(OBJDIR)
>rm -f bin/test_protocol bin/test_state bin/test_config \
      bin/test_command bin/test_aml bin/test_aml_commands \
      bin/sweetdawn

rebuild: clean all