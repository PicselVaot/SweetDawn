#ifndef AML_TRANSPORT_H
#define AML_TRANSPORT_H

/*
 * aml/transport.h — Liaison Bluetooth RFCOMM avec l'AML005
 *
 * Ce module gère :
 *   - la connexion RFCOMM ;
 *   - l'envoi de trames ;
 *   - la réception de trames complètes ;
 *   - la fermeture de la connexion.
 *
 * La construction des trames et leur extraction du flux reçu
 * restent gérées par aml/protocol.c.
 *
 * Les fonctions sont bloquantes, mais toujours avec un délai maximal :
 * aucune ne peut figer le programme indéfiniment.
 */

#include <stddef.h>
#include <stdint.h>

#include "core/config.h"
#include "aml/protocol.h"

/*
 * Codes d'erreur renvoyés par les fonctions de transport.
 *
 * Ils sont tous négatifs : les appelants qui testent « < 0 » ou
 * « != 0 » continuent de fonctionner. Les distinguer permet au service
 * de décider : un délai dépassé peut être retenté, une liaison perdue
 * impose de se reconnecter.
 */
#define AML_ERR_GENERIC       (-1) /* arguments invalides, erreur diverse   */
#define AML_ERR_TIMEOUT       (-2) /* aucune trame dans le délai imparti    */
#define AML_ERR_DISCONNECTED  (-3) /* liaison fermée ou perdue              */
#define AML_ERR_IO            (-4) /* erreur système sur le socket          */
#define AML_ERR_PROTOCOL      (-5) /* tampon saturé, trame inexploitable    */

/* Délai maximal accordé à l'établissement de la liaison RFCOMM. */
#define AML_CONNECT_TIMEOUT_MS 8000

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
 *
 * L'attente est bornée par AML_CONNECT_TIMEOUT_MS : un AML éteint ne
 * bloque donc pas le service indéfiniment.
 */
int aml_connect(
    AmlConnection *connection,
    const AmlConfig *config
);

/*
 * Envoie tous les octets d'une trame sur la connexion.
 *
 * Retour : 0, AML_ERR_DISCONNECTED, AML_ERR_IO ou AML_ERR_GENERIC.
 * Ne déclenche jamais SIGPIPE.
 */
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
 *   0                    : trame reçue ;
 *   AML_ERR_TIMEOUT      : délai dépassé (la liaison est peut-être saine) ;
 *   AML_ERR_DISCONNECTED : l'AML a fermé la liaison ou elle est perdue ;
 *   AML_ERR_IO / AML_ERR_PROTOCOL / AML_ERR_GENERIC : autres erreurs.
 */
int aml_receive(
    AmlConnection *connection,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_length,
    int timeout_ms
);

/*
 * Jette tout ce qui est en attente de lecture : trames partielles
 * du parseur et octets déjà arrivés sur le socket.
 *
 * À appeler avant d'émettre une requête, pour qu'une réponse tardive
 * d'un échange précédent ne soit pas prise pour la réponse suivante.
 *
 * Retourne le nombre d'octets écartés (0 si la liaison était propre).
 */
size_t aml_flush(AmlConnection *connection);

/* Ferme la connexion et remet son descripteur à -1. */
void aml_disconnect(AmlConnection *connection);

#endif /* AML_TRANSPORT_H */