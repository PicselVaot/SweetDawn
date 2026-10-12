/*
 * aml/protocol.c — Construction et extraction des trames AML005
 *
 * Les commandes fournissent uniquement le code de commande et
 * leurs paramètres métier.
 *
 * Ce module ajoute les champs techniques nécessaires :
 *   - en-tête ;
 *   - longueur ;
 *   - checksum ;
 *   - marqueur 0x55 ;
 *   - terminaison 0x0D 0x0A.
 */

#include "aml/protocol.h"

#include <limits.h>
#include <string.h>

/*
 * Taille totale des champs fixes qui entourent le contenu interne :
 *
 * FE EF 0A LL AB AA [contenu interne] 0D 0A
 *
 * Six octets avant le contenu interne et deux après.
 */
#define AML_FRAME_FIXED_SIZE 8

int protocol_build_request(
    uint8_t request_code,
    const uint8_t *arguments,
    size_t arguments_length,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_length)
{
    size_t internal_length;
    size_t frame_length;
    size_t checksum_index;
    size_t i;
    uint8_t checksum = 0;

    /* Vérifier les pointeurs et les arguments fournis. */
    if (output == NULL || output_length == NULL)
        return -1;

    if (arguments_length > 0 && arguments == NULL)
        return -1;

    /*
     * Le contenu interne comprend :
     *
     * 1 octet : longueur interne ;
     * 1 octet : code de commande ;
     * N octets : paramètres métier ;
     * 1 octet : checksum ;
     * 1 octet : marqueur 0x55.
     */
    if (arguments_length > SIZE_MAX - 4)
        return -1;

    internal_length = arguments_length + 4;

    /*
     * La longueur interne est stockée sur un octet.
     * La longueur déclarée de la trame l'est également.
     */
    if (internal_length - 1 > UINT8_MAX)
        return -1;

    if (internal_length > UINT8_MAX - 2)
        return -1;

    /*
     * La trame complète contient six octets avant le contenu interne
     * et deux octets de terminaison après celui-ci.
     */
    if (internal_length > SIZE_MAX - AML_FRAME_FIXED_SIZE)
        return -1;

    frame_length = internal_length + AML_FRAME_FIXED_SIZE;

    if (output_capacity < frame_length)
        return -1;

    /*
     * Construire l'en-tête.
     *
     * L'octet 3 contient la longueur totale moins les quatre
     * premiers octets, conformément aux trames observées.
     */
    output[0] = AML_FRAME_START_1;
    output[1] = AML_FRAME_START_2;
    output[2] = AML_FRAME_TYPE_REQUEST;
    output[3] = (uint8_t)(frame_length - 4);
    output[4] = AML_FRAME_HEADER;
    output[5] = AML_FRAME_HEADER_REQUEST;

    /*
     * Construire le contenu interne.
     *
     * La longueur interne correspond au nombre d'octets suivant
     * cet octet jusqu'à la fin du contenu interne.
     */
    output[6] = (uint8_t)(internal_length - 1);
    output[7] = request_code;

    /* Copier uniquement les paramètres métier de la commande. */
    if (arguments_length > 0)
        memcpy(&output[8], arguments, arguments_length);

    /*
     * Le checksum vient immédiatement après les paramètres.
     *
     * Il est calculé par XOR à partir de l'octet de longueur interne
     * et jusqu'au dernier paramètre inclus.
     */
    checksum_index = 8 + arguments_length;

    for (i = 6; i < checksum_index; i++)
        checksum ^= output[i];

    output[checksum_index] = checksum;

    /* Ajouter le marqueur interne qui précède la terminaison. */
    output[checksum_index + 1] = AML_FRAME_END;

    /* Terminer la trame par CR LF. */
    output[frame_length - 2] = AML_FRAME_CR;
    output[frame_length - 1] = AML_FRAME_LF;

    /* Communiquer à l'appelant la taille de la trame construite. */
    *output_length = frame_length;

    return 0;
}

/*
 * Initialise le parseur.
 *
 * Aucun octet n'est encore en attente de traitement.
 */
void protocol_parser_init(ProtocolParser *parser)
{
    if (parser == NULL)
        return;

    parser->length = 0;
}

/*
 * Ajoute les octets reçus au tampon du parseur.
 *
 * Cette fonction n'essaie pas de reconstruire les trames :
 * elle accumule simplement les données jusqu'à ce que
 * protocol_parser_next() puisse extraire une trame complète.
 */
int protocol_parser_feed(
    ProtocolParser *parser,
    const uint8_t *data,
    size_t data_length)
{
    /* Vérifier les arguments avant toute manipulation mémoire. */
    if (parser == NULL)
        return -1;

    if (data_length > 0 && data == NULL)
        return -1;

    /* Refuser l'ajout si les données dépassent la capacité restante. */
    if (data_length > AML_PROTOCOL_BUFFER_SIZE - parser->length)
        return -1;

    /* Ajouter les nouveaux octets à la suite des précédents. */
    if (data_length > 0) {
        memcpy(
            parser->data + parser->length,
            data,
            data_length
        );

        parser->length += data_length;
    }

    return 0;
}

/*
 * Retire les premiers octets du tampon.
 *
 * Après le retrait, les octets restants sont déplacés au début
 * du tampon. Cette opération permet de conserver les données
 * reçues après une trame, notamment lorsqu'une lecture Bluetooth
 * contient plusieurs trames.
 */
static void protocol_parser_consume(
    ProtocolParser *parser,
    size_t count)
{
    if (count >= parser->length) {
        parser->length = 0;
        return;
    }

    memmove(
        parser->data,
        parser->data + count,
        parser->length - count
    );

    parser->length -= count;
}

/*
 * Extrait une trame complète si elle est disponible.
 *
 * Une trame n'est extraite que si :
 *   - son marqueur de début FE EF est présent ;
 *   - son en-tête correspond à une requête ou une réponse observée ;
 *   - sa longueur déclarée est cohérente avec la taille minimale ;
 *   - les trois derniers octets correspondent à 55 0D 0A.
 *
 * Cette validation est structurelle : elle ne vérifie pas encore
 * le checksum des réponses.
 */
int protocol_parser_next(
    ProtocolParser *parser,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_length)
{
    size_t frame_length;
    size_t i;

    if (parser == NULL || output == NULL || output_length == NULL)
        return -1;

    /* Aucune trame ne peut être extraite si le tampon est vide. */
    if (parser->length == 0)
        return 0;

    /*
     * Chercher le début d'une trame.
     *
     * On conserve les données à partir du premier FE EF trouvé.
     * Si aucun début n'est trouvé, on conserve éventuellement
     * un FE final : il pourrait être suivi de EF à la prochaine lecture.
     */
    for (i = 0; i + 1 < parser->length; i++) {
        if (parser->data[i] == AML_FRAME_START_1 &&
            parser->data[i + 1] == AML_FRAME_START_2) {
            break;
        }
    }

    if (i + 1 >= parser->length) {
        /*
         * Aucun début complet trouvé.
         *
         * Si le dernier octet vaut FE, le garder pour pouvoir
         * reconnaître FE EF lorsque la suite arrivera.
         */
        if (parser->data[parser->length - 1] == AML_FRAME_START_1) {
            parser->data[0] = AML_FRAME_START_1;
            parser->length = 1;
        } else {
            parser->length = 0;
        }

        return 0;
    }

    /* Éliminer le bruit éventuellement présent avant FE EF. */
    if (i > 0)
        protocol_parser_consume(parser, i);

    /*
     * L'en-tête et l'octet de longueur occupent quatre octets.
     * Si nous n'en avons pas encore assez, attendre la prochaine lecture.
     */
    if (parser->length < 4)
        return 0;

    /*
     * Les trames observées utilisent :
     *   0A : requête ;
     *   0C : réponse.
     *
     * Une autre valeur indique que ce candidat n'est pas une trame
     * reconnue. On abandonne un octet puis on recherche à nouveau.
     */
    if (parser->data[2] != AML_FRAME_TYPE_REQUEST &&
        parser->data[2] != AML_FRAME_TYPE_RESPONSE) {
        protocol_parser_consume(parser, 1);
        return protocol_parser_next(
            parser, output, output_capacity, output_length
        );
    }

    /*
     * Les octets 4 et 5 identifient le type de trame :
     *   AB AA : requête ;
     *   AB BB : réponse.
     */
    if (parser->length >= 6) {
        uint8_t expected_header =
            parser->data[2] == AML_FRAME_TYPE_REQUEST
            ? AML_FRAME_HEADER_REQUEST
            : AML_FRAME_HEADER_RESPONSE;

        if (parser->data[4] != AML_FRAME_HEADER ||
            parser->data[5] != expected_header) {
            protocol_parser_consume(parser, 1);
            return protocol_parser_next(
                parser, output, output_capacity, output_length
            );
        }
    } else {
        return 0;
    }

    /*
     * La longueur totale observée est égale à l'octet 3 plus quatre.
     */
    frame_length = (size_t)parser->data[3] + 4;

    /*
     * Les trames connues font au minimum 12 octets.
     * Une longueur plus petite est donc considérée comme invalide.
     */
    if (frame_length < 12 ||
        frame_length > AML_PROTOCOL_MAX_FRAME_SIZE) {
        protocol_parser_consume(parser, 1);
        return protocol_parser_next(
            parser, output, output_capacity, output_length
        );
    }

    /* La trame est peut-être encore incomplète : attendre la suite. */
    if (parser->length < frame_length)
        return 0;

    /*
     * Vérifier la terminaison de la trame candidate.
     *
     * On ne valide volontairement pas le checksum ici :
     * sa règle pour les réponses n'est pas encore confirmée.
     */
    if (parser->data[frame_length - 3] != AML_FRAME_END ||
        parser->data[frame_length - 2] != AML_FRAME_CR ||
        parser->data[frame_length - 1] != AML_FRAME_LF) {
        /*
         * La trame candidate est invalide.
         * On retire un seul octet pour permettre de retrouver
         * un éventuel début de trame situé à l'intérieur.
         */
        protocol_parser_consume(parser, 1);

        return protocol_parser_next(
            parser, output, output_capacity, output_length
        );
    }

    /* Vérifier que la trame tient dans le tampon de sortie. */
    if (output_capacity < frame_length)
        return -1;

    /* Copier la trame complète vers le tampon de sortie. */
    memcpy(output, parser->data, frame_length);
    *output_length = frame_length;

    /*
     * Retirer uniquement la trame extraite.
     * Les éventuelles trames suivantes restent en attente.
     */
    protocol_parser_consume(parser, frame_length);

    return 1;
}

