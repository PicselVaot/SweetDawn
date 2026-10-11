#ifndef AML_H
#define AML_H

/*
 * aml.h — Interface de communication Bluetooth avec l'AML005
 *
 * Ce module gère :
 *   - la connexion RFCOMM ;
 *   - l'envoi de trames ;
 *   - la réception de trames complètes ;
 *   - la fermeture de la connexion.
 *
 * La construction des trames et leur extraction du flux reçu
 * restent gérées par protocol.c.
 */

#include <stddef.h>
#include <stdint.h>

#include "config.h"
#include "protocol.h"

/*
 * Représente une connexion Bluetooth avec l'AML005.
 *
 * socket_fd :
 *   Descripteur du socket Bluetooth. -1 signifie déconnecté.
 *
 * parser :
 *   Accumule les octets reçus et permet d'extraire les trames
 *   même lorsque les lectures Bluetooth sont fragmentées.
 */
typedef struct {
    int socket_fd;
    ProtocolParser parser;
} AmlConnection;

/*
 * Ouvre une connexion RFCOMM vers l'appareil configuré.
 *
 * Les adresses Bluetooth et le canal RFCOMM proviennent
 * de la configuration de l'appareil.
 */
int aml_connect(
    AmlConnection *connection,
    const AmlConfig *config
);

/* Envoie tous les octets d'une trame sur la connexion. */
int aml_send(
    AmlConnection *connection,
    const uint8_t *data,
    size_t length
);

/*
 * Attend une trame complète.
 *
 * timeout_ms :
 *   Délai maximal d'attente en millisecondes.
 *   Une valeur négative signifie attendre indéfiniment.
 *
 * Retour :
 *   0  : trame reçue ;
 *  -1  : erreur ou délai dépassé.
 */
int aml_receive(
    AmlConnection *connection,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_length,
    int timeout_ms
);

/* Ferme la connexion et remet son descripteur à -1. */
void aml_disconnect(AmlConnection *connection);

#endif /* AML_H */