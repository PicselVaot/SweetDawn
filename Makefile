# ============================================================================
#  SweetDawn — Makefile
# ============================================================================
#
#  make                    Compile les programmes et tous les tests.
#  make test               Lance les tests automatiques (aucun Bluetooth requis).
#  make install-service    Installe et démarre le service systemd (sudo).
#  make uninstall-service  Arrête et retire le service systemd (sudo).
#  make clean              Supprime tout ce qui a été compilé (le dossier bin/).
#
#  Les programmes produits :
#    bin/sweetdawn          Diagnostic ponctuel : lit l'AML puis s'arrête.
#    bin/sweetdawn-service  Service persistant, piloté via un socket Unix.
#    bin/sweetdawn-ctl      Client du service : une commande, une réponse.
#
#  Tout ce qui est compilé va dans bin/ ; les fichiers objets vont dans
#  bin/obj/, en reproduisant l'arborescence de src/ et test/.
#
# ============================================================================

CC       := gcc
CFLAGS   := -std=c11 -Wall -Wextra -Wpedantic -O2

# -Isrc permet d'écrire #include "aml/protocol.h" depuis n'importe où.
# -MMD -MP demandent à gcc de noter, pour chaque .o, les .h qu'il utilise :
# modifier un header recompile alors automatiquement les bons fichiers.
CPPFLAGS := -Isrc -MMD -MP $(shell pkg-config --cflags json-c)

JSON_LIBS      := $(shell pkg-config --libs json-c)
BLUETOOTH_LIBS := -lbluetooth

BIN := bin
OBJ := bin/obj


# ----------------------------------------------------------------------------
#  Modules
# ----------------------------------------------------------------------------
#  Un module = un dossier de src/. Chaque liste donne les objets qui le
#  composent ; les programmes et les tests piochent dans ces listes.

# Dialogue avec l'AML005 : format des trames et transport Bluetooth.
AML_PROTOCOL  := $(OBJ)/aml/protocol.o
AML_TRANSPORT := $(OBJ)/aml/transport.o

# Cœur applicatif : configuration, état mémorisé, supervision de la liaison,
# et une fonction par opération de l'AML (control).
CORE_CONFIG    := $(OBJ)/core/config.o
CORE_STATE     := $(OBJ)/core/state.o
CORE_SWEETDAWN := $(OBJ)/core/sweetdawn.o
CORE_CONTROL   := $(OBJ)/core/control.o

# Arbre des commandes AML (Settings, Alarm, Light, ...).
COMMANDS := $(OBJ)/command/command.o \
            $(OBJ)/command/registry.o \
            $(OBJ)/command/aml_commands.o

# Serveur IPC : socket Unix et messages JSON.
IPC := $(OBJ)/ipc/ipc_protocol.o \
       $(OBJ)/ipc/ipc_server.o

# Commandes IPC du service : relie l'arbre des commandes à core/control.
HANDLERS := $(OBJ)/app/handlers.o

# Faux AML pour les tests : remplace aml/transport.c et core/config.c.
FAKE_AML := $(OBJ)/test/fake_aml.o


# ----------------------------------------------------------------------------
#  Programmes
# ----------------------------------------------------------------------------

PROGRAMS := $(BIN)/sweetdawn $(BIN)/sweetdawn-service $(BIN)/sweetdawn-ctl

TESTS := $(BIN)/test_protocol \
         $(BIN)/test_state \
         $(BIN)/test_config \
         $(BIN)/test_command \
         $(BIN)/test_aml_commands \
         $(BIN)/test_sweetdawn \
         $(BIN)/test_handlers \
         $(BIN)/test_ipc

.PHONY: all test clean rebuild install-service uninstall-service

all: $(PROGRAMS) $(TESTS)

# Diagnostic : n'utilise pas le cœur SweetDawn, seulement le transport.
$(BIN)/sweetdawn: $(OBJ)/app/cli.o \
                  $(AML_PROTOCOL) $(AML_TRANSPORT) \
                  $(CORE_CONFIG) $(CORE_STATE)
	$(CC) $^ $(JSON_LIBS) $(BLUETOOTH_LIBS) -o $@

# Service : cœur complet + serveur IPC.
$(BIN)/sweetdawn-service: $(OBJ)/app/service.o $(HANDLERS) \
                          $(AML_PROTOCOL) $(AML_TRANSPORT) \
                          $(CORE_CONFIG) $(CORE_STATE) $(CORE_SWEETDAWN) \
                          $(CORE_CONTROL) $(COMMANDS) $(IPC)
	$(CC) $^ $(JSON_LIBS) $(BLUETOOTH_LIBS) -o $@

# Client : ne parle qu'au socket du service, jamais au Bluetooth.
$(BIN)/sweetdawn-ctl: $(OBJ)/app/ctl.o
	$(CC) $^ $(JSON_LIBS) -o $@


# ----------------------------------------------------------------------------
#  Tests
# ----------------------------------------------------------------------------
#  Chaque test est un petit programme : test/test_X.c + les modules testés.

$(BIN)/test_protocol: $(OBJ)/test/test_protocol.o $(AML_PROTOCOL)
	$(CC) $^ -o $@

$(BIN)/test_state: $(OBJ)/test/test_state.o $(CORE_STATE)
	$(CC) $^ -o $@

$(BIN)/test_config: $(OBJ)/test/test_config.o $(CORE_CONFIG)
	$(CC) $^ $(JSON_LIBS) -o $@

$(BIN)/test_command: $(OBJ)/test/test_command.o $(COMMANDS)
	$(CC) $^ -o $@

$(BIN)/test_aml_commands: $(OBJ)/test/test_aml_commands.o $(COMMANDS)
	$(CC) $^ -o $@

# Ces deux tests parlent au faux AML (FAKE_AML) : transport.o et config.o
# ne sont donc volontairement PAS liés ici.
$(BIN)/test_sweetdawn: $(OBJ)/test/test_sweetdawn.o $(FAKE_AML) \
                       $(CORE_SWEETDAWN) $(CORE_STATE) \
                       $(AML_PROTOCOL) $(COMMANDS)
	$(CC) $^ -o $@

$(BIN)/test_handlers: $(OBJ)/test/test_handlers.o $(FAKE_AML) $(HANDLERS) \
                      $(CORE_CONTROL) $(CORE_SWEETDAWN) $(CORE_STATE) \
                      $(AML_PROTOCOL) $(COMMANDS)
	$(CC) $^ $(JSON_LIBS) -o $@

$(BIN)/test_ipc: $(OBJ)/test/test_ipc.o $(IPC)
	$(CC) $^ $(JSON_LIBS) -o $@

# Lance chaque test l'un après l'autre ; s'arrête au premier échec.
test: $(TESTS)
	@for t in $(TESTS); do \
		echo; echo "==== $$t"; \
		./$$t || exit 1; \
	done
	@echo; echo "Tous les tests sont passés."


# ----------------------------------------------------------------------------
#  Règles génériques
# ----------------------------------------------------------------------------

# src/aml/protocol.c  ->  bin/obj/aml/protocol.o
$(OBJ)/%.o: src/%.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

# test/test_state.c  ->  bin/obj/test/test_state.o
$(OBJ)/test/%.o: test/%.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

# Les programmes et tests finaux doivent avoir leur dossier bin/.
$(PROGRAMS) $(TESTS): | $(BIN)

$(BIN):
	mkdir -p $@

clean:
	rm -rf $(BIN)


# ----------------------------------------------------------------------------
#  Service systemd
# ----------------------------------------------------------------------------
#  Le service exécute bin/sweetdawn-service depuis ce dossier : après un
#  nouveau `make`, il suffit de `sudo systemctl restart sweetdawn`.
#  sweetdawn-ctl est copié dans /usr/local/bin pour être tapé de partout.

UNIT := /etc/systemd/system/sweetdawn.service

install-service: $(BIN)/sweetdawn-service $(BIN)/sweetdawn-ctl
	sed -e 's|@USER@|$(USER)|g' -e 's|@DIR@|$(CURDIR)|g' \
	    systemd/sweetdawn.service | sudo tee $(UNIT) > /dev/null
	sudo install -m 755 $(BIN)/sweetdawn-ctl /usr/local/bin/sweetdawn-ctl
	sudo systemctl daemon-reload
	sudo systemctl enable --now sweetdawn
	@echo "Service installé : systemctl status sweetdawn"

uninstall-service:
	-sudo systemctl disable --now sweetdawn
	sudo rm -f $(UNIT) /usr/local/bin/sweetdawn-ctl
	sudo systemctl daemon-reload

rebuild: clean
	$(MAKE) all

# Dépendances vers les headers, générées par -MMD (fichiers .d).
-include $(shell find $(OBJ) -name '*.d' 2>/dev/null)
