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
 * Codes des commandes.
 *
 * Source : application Android d'origine (SunBeam, décompilée), classe
 * ResponseCode et classes *Cmd. Le détail des données de chaque
 * commande est dans docs/PROTOCOL.md.
 *
 * Chaque requête AML_REQ_X reçoit, si elle en a une, une réponse
 * AML_RSP_X portant un code différent : c'est ce code qui permet de
 * reconnaître la bonne réponse. Les commandes sans AML_RSP_X ne
 * reçoivent aucune réponse (l'application d'origine n'en attend pas).
 */

/* Appareil et réglages généraux. */
#define AML_REQ_STATUS             0x00  /* autorisation de contrôle      */
#define AML_RSP_STATUS             0xF0
#define AML_REQ_SYNC_TIME          0x01  /* mise à l'heure                */
#define AML_RSP_SYNC_TIME          0xF1
#define AML_REQ_WRITE_SETTINGS     0x02  /* écrit les 5 réglages          */
#define AML_RSP_WRITE_SETTINGS     0xF2
#define AML_REQ_READ_SETTINGS      0x12  /* lit les 5 réglages            */
#define AML_RSP_READ_SETTINGS      0xE2

/* Alarmes. */
#define AML_REQ_ALARM              0x03  /* lecture ou écriture d'une alarme */
#define AML_RSP_ALARM              0xF3
#define AML_REQ_ALARM_STATUS       0x25
#define AML_RSP_ALARM_STATUS       0xC5
#define AML_REQ_ALARM_SNOOZE       0x28
#define AML_RSP_ALARM_SNOOZE       0xC8
#define AML_REQ_ALARM_STOP         0x29
#define AML_RSP_ALARM_STOP         0xC9

/* Éclairage : Light (lampe principale) et Mood (ambiance). */
#define AML_REQ_LIGHT_QUERY        0x30
#define AML_RSP_LIGHT_QUERY        0xD0
#define AML_REQ_LIGHT_BRIGHTNESS   0x31  /* sans réponse */
#define AML_REQ_MOOD_COLOR         0x32  /* sans réponse */
#define AML_REQ_LIGHT_TIMER        0x33  /* sans réponse */
#define AML_REQ_MOOD_SCENE         0x34  /* sans réponse */
#define AML_REQ_LIGHT_OFF          0x35  /* sans réponse */
#define AML_REQ_LIGHT_TIMER_OFF    0x36  /* sans réponse */
#define AML_REQ_LIGHT_ON           0x37  /* sans réponse */
#define AML_REQ_LIGHT_TIMER_ON     0x38  /* sans réponse */

/* Cible des commandes d'éclairage (premier octet de données). */
#define AML_LIGHT_TARGET_LIGHT     0x01
#define AML_LIGHT_TARGET_MOOD      0x02

/* Radio FM. */
#define AML_REQ_RADIO_LIST         0x04  /* liste des présélections */
#define AML_RSP_RADIO_LIST         0xF4
#define AML_REQ_RADIO_QUERY        0x07
#define AML_RSP_RADIO_QUERY        0xF7
#define AML_REQ_RADIO_ENABLE       0x08
#define AML_RSP_RADIO_ENABLE       0xF8
#define AML_REQ_RADIO_PRESET       0x09
#define AML_RSP_RADIO_PRESET       0xF9
#define AML_REQ_RADIO_TUNE         0x0A
#define AML_RSP_RADIO_TUNE         0xFA
#define AML_REQ_RADIO_VOLUME       0x0B  /* sans réponse */
#define AML_REQ_RADIO_TIMER_ENABLE 0x0C  /* sans réponse */
#define AML_REQ_RADIO_TIMER        0x0D  /* sans réponse */
#define AML_REQ_RADIO_SAVE_PRESET  0x0E
#define AML_RSP_RADIO_SAVE_PRESET  0xFE

/* Haut-parleur Bluetooth (Media Player). */
#define AML_REQ_A2DP_STATUS        0x0F
#define AML_RSP_A2DP_STATUS        0xFF
#define AML_REQ_SPEAKER_ON         0x10
#define AML_RSP_SPEAKER_ON         0xE0
#define AML_REQ_MUSIC_VOLUME       0x14  /* sans réponse */
#define AML_REQ_MUSIC_TIMER_ENABLE 0x15
#define AML_RSP_MUSIC_TIMER_ENABLE 0xE5
#define AML_REQ_MUSIC_TIMER        0x16  /* sans réponse */
#define AML_REQ_MUSIC_QUERY        0x17
#define AML_RSP_MUSIC_QUERY        0xE7
#define AML_REQ_SPEAKER_OFF        0x24  /* sans réponse */

/*
 * Messages spontanés : l'AML les envoie de lui-même, sans requête.
 * Pas encore exploités (étape 5 du TODO).
 */
#define AML_EVT_SUNRISE_START      0xC6
#define AML_EVT_ALARM_RING         0xC7
#define AML_EVT_LIGHT_REFRESH      0xCA
#define AML_EVT_LIGHT_TIMER_END    0xEB
#define AML_EVT_MOOD_TIMER_END     0xEC
#define AML_EVT_RADIO_TIMER_END    0xED
#define AML_EVT_MUSIC_TIMER_END    0xEE

/* Argument fixe de la requête de statut ; 0x16 est aussi la valeur
 * d'autorisation renvoyée quand le contrôle est accordé. */
#define AML_STATUS_ARG             0x16
#define AML_AUTHORIZATION_GRANTED  0x16

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