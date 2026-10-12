#ifndef AML_PROTOCOL_H
#define AML_PROTOCOL_H

/*
 * aml/protocol.h — Format des trames échangées avec l'AML005
 *
 * Ce module permet :
 *   - de construire les trames de requête ;
 *   - de recevoir un flux d'octets ;
 *   - d'en extraire les trames complètes.
 *
 * Il ne fait aucune entrée/sortie : le Bluetooth est géré par
 * aml/transport.c. On peut donc le tester sans matériel.
 *
 * Anatomie d'une trame (valeurs observées sur le matériel réel) :
 *
 *   octet :  0  1  2  3  4  5  6  7  8 ...       n-3 n-2 n-1
 *           FE EF TT LL AB KK IL CC [données] XX  55  0D  0A
 *
 *   FE EF : début de trame
 *   TT    : 0A pour une requête, 0C pour une réponse
 *   LL    : longueur totale de la trame moins 4
 *   AB KK : AB AA pour une requête, AB BB pour une réponse
 *   IL    : longueur interne (code + données + checksum + 55)
 *   CC    : code de la commande (ou de la réponse)
 *   XX    : checksum, XOR des octets de IL jusqu'à la dernière donnée
 *   55    : marqueur de fin interne
 *   0D 0A : fin de trame (CR LF)
 *
 * Exemple, demande de statut :  FE EF 0A 09 AB AA 04 00 16 12 55 0D 0A
 *          et sa réponse     :  FE EF 0C 09 AB BB 04 F0 16 E2 55 0D 0A
 */

#include <stddef.h>
#include <stdint.h>

/* Délimiteurs de trame observés dans les communications AML005. */
#define AML_FRAME_START_1  0xFE
#define AML_FRAME_START_2  0xEF
#define AML_FRAME_END      0x55
#define AML_FRAME_CR       0x0D
#define AML_FRAME_LF       0x0A

/* Octet 2 : sens de la trame. */
#define AML_FRAME_TYPE_REQUEST   0x0A
#define AML_FRAME_TYPE_RESPONSE  0x0C

/* Octets 4 et 5 : en-tête, AB AA (requête) ou AB BB (réponse). */
#define AML_FRAME_HEADER          0xAB
#define AML_FRAME_HEADER_REQUEST  0xAA
#define AML_FRAME_HEADER_RESPONSE 0xBB

/* Position du code de commande, puis des données, dans une trame. */
#define AML_FRAME_CODE_OFFSET  7
#define AML_FRAME_DATA_OFFSET  8

/*
 * Commandes connues.
 *
 * Chaque requête reçoit une réponse portant un code différent :
 * c'est ce code qui permet de reconnaître la bonne réponse.
 *
 * Statut : la requête porte un argument (0x16). La réponse contient
 * un octet d'autorisation : 0x16 (22) signifie que le contrôle de
 * l'AML est accordé.
 *
 * Paramètres : la réponse contient cinq octets, dans l'ordre
 * display, date, time, feedback, fade (voir core/state.h).
 */
#define AML_REQUEST_STATUS        0x00
#define AML_REQUEST_STATUS_ARG    0x16
#define AML_RESPONSE_STATUS       0xF0

#define AML_REQUEST_SETTINGS      0x12
#define AML_RESPONSE_SETTINGS     0xE2

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

#endif /* AML_PROTOCOL_H */