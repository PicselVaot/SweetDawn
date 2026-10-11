#ifndef PROTOCOL_H
#define PROTOCOL_H

/*
 * protocol.h — Interface du protocole de communication AML005
 *
 * Ce module permet :
 *   - de construire les trames de requête ;
 *   - de recevoir un flux d'octets ;
 *   - d'en extraire les trames complètes.
 */

#include <stddef.h>
#include <stdint.h>

/* Délimiteurs de trame observés dans les communications AML005. */
#define AML_FRAME_START_1  0xFE
#define AML_FRAME_START_2  0xEF
#define AML_FRAME_END      0x55
#define AML_FRAME_CR       0x0D
#define AML_FRAME_LF       0x0A

/*
 * Taille maximale d'une trame :
 *
 * L'octet de longueur peut prendre une valeur comprise entre 0 et 255.
 * La longueur totale est égale à cet octet plus quatre.
 *
 * Une trame peut donc atteindre 259 octets.
 */
#define AML_PROTOCOL_MAX_FRAME_SIZE 259

/*
 * Capacité du tampon qui accumule les octets reçus.
 *
 * Il permet notamment de conserver plusieurs trames en attente
 * d'extraction. Si le tampon est plein, l'appelant doit extraire
 * les trames disponibles avant d'ajouter d'autres données.
 */
#define AML_PROTOCOL_BUFFER_SIZE 1024

/*
 * Tampon de réception du protocole.
 *
 * data contient les octets reçus qui n'ont pas encore été extraits
 * sous forme de trames complètes.
 */
typedef struct {
    uint8_t data[AML_PROTOCOL_BUFFER_SIZE];
    size_t length;
} ProtocolParser;

/*
 * Construit une requête AML005 à partir d'un code de commande
 * et de ses paramètres.
 *
 * Retour :
 *   0  : trame construite ;
 *  -1  : arguments invalides ou capacité insuffisante.
 */
int protocol_build_request(
    uint8_t request_code,
    const uint8_t *arguments,
    size_t arguments_length,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_length
);

/*
 * Initialise le parseur de réception.
 *
 * À appeler une fois avant de commencer à recevoir des données.
 */
void protocol_parser_init(ProtocolParser *parser);

/*
 * Ajoute des octets au tampon de réception.
 *
 * Les données incomplètes sont conservées pour les prochaines lectures.
 *
 * Retour :
 *   0  : données ajoutées ;
 *  -1  : arguments invalides ou tampon insuffisant.
 */
int protocol_parser_feed(
    ProtocolParser *parser,
    const uint8_t *data,
    size_t data_length
);

/*
 * Extrait la prochaine trame complète du tampon.
 *
 * Les octets qui ne correspondent pas à une trame exploitable sont
 * écartés progressivement afin de retrouver le prochain début de trame.
 *
 * Retour :
 *   1  : une trame complète a été extraite ;
 *   0  : aucune trame complète disponible pour le moment ;
 *  -1  : arguments invalides ou tampon de sortie insuffisant.
 *
 * Si une trame est extraite, output_length contient sa taille.
 * Les éventuelles trames suivantes restent dans le parseur.
 */
int protocol_parser_next(
    ProtocolParser *parser,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_length
);

#endif /* PROTOCOL_H */