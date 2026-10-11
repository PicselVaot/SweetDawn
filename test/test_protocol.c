/*
 * test_protocol.c — Tests unitaires du protocole AML005
 *
 * Ces tests vérifient :
 *   1. La construction de la requête de statut.
 *   2. La construction de la requête de paramètres.
 *   3. La réception d'une trame en plusieurs morceaux.
 *   4. L'extraction de plusieurs trames reçues ensemble.
 *   5. La recherche d'une trame après des octets parasites.
 *
 * Compilation depuis la racine du projet :
 *
 * gcc -Wall -Wextra -Wpedantic -std=c11 \
 *     -Isrc src/protocol.c test/test_protocol.c \
 *     -o bin/test_protocol
 *
 * Exécution :
 *
 * ./bin/test_protocol
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "protocol.h"

/*
 * Affiche une trame sous forme hexadécimale.
 * Cette fonction facilite le diagnostic en cas de problème.
 */
static void afficher_trame(const uint8_t *trame, size_t longueur)
{
    size_t i;

    printf("Trame produite (%zu octets) : ", longueur);

    for (i = 0; i < longueur; i++)
        printf("%02X%s", trame[i], i + 1 < longueur ? " " : "");

    printf("\n");
}

/*
 * Test 1 : construction de la requête de statut.
 *
 * Requête observée :
 * FE EF 0A 09 AB AA 04 00 16 12 55 0D 0A
 *
 * Code de commande : 0x00
 * Paramètre        : 0x16
 */
static void test_requete_statut(void)
{
    const uint8_t parametres[] = {0x16};

    const uint8_t trame_attendue[] = {
        0xFE, 0xEF, 0x0A, 0x09, 0xAB, 0xAA,
        0x04, 0x00, 0x16, 0x12, 0x55, 0x0D, 0x0A
    };

    uint8_t trame[AML_PROTOCOL_MAX_FRAME_SIZE];
    size_t longueur = 0;

    int resultat = protocol_build_request(
        0x00,
        parametres,
        sizeof(parametres),
        trame,
        sizeof(trame),
        &longueur
    );

    assert(resultat == 0);
    assert(longueur == sizeof(trame_attendue));

    afficher_trame(trame, longueur);

    assert(memcmp(
        trame,
        trame_attendue,
        sizeof(trame_attendue)
    ) == 0);

    printf("OK : Requete de statut\n\n");
}

/*
 * Test 2 : construction de la requête de paramètres.
 *
 * Requête observée :
 * FE EF 0A 08 AB AA 03 12 11 55 0D 0A
 *
 * Code de commande : 0x12
 * Aucun paramètre métier.
 */
static void test_requete_parametres(void)
{
    const uint8_t trame_attendue[] = {
        0xFE, 0xEF, 0x0A, 0x08, 0xAB, 0xAA,
        0x03, 0x12, 0x11, 0x55, 0x0D, 0x0A
    };

    uint8_t trame[AML_PROTOCOL_MAX_FRAME_SIZE];
    size_t longueur = 0;

    int resultat = protocol_build_request(
        0x12,
        NULL,
        0,
        trame,
        sizeof(trame),
        &longueur
    );

    assert(resultat == 0);
    assert(longueur == sizeof(trame_attendue));

    afficher_trame(trame, longueur);

    assert(memcmp(
        trame,
        trame_attendue,
        sizeof(trame_attendue)
    ) == 0);

    printf("OK : Requete des parametres\n\n");
}

/*
 * Test 3 : réception d'une réponse fragmentée.
 *
 * La réponse réelle est séparée artificiellement en deux morceaux.
 * Le parseur ne doit pas produire de trame avant d'avoir reçu
 * la totalité des octets.
 */
static void test_reponse_fragmentee(void)
{
    const uint8_t premiere_partie[] = {
        0xFE, 0xEF, 0x0C, 0x09, 0xAB, 0xBB
    };

    const uint8_t seconde_partie[] = {
        0x04, 0xF0, 0x16, 0xE2, 0x55, 0x0D, 0x0A
    };

    const uint8_t reponse_attendue[] = {
        0xFE, 0xEF, 0x0C, 0x09, 0xAB, 0xBB,
        0x04, 0xF0, 0x16, 0xE2, 0x55, 0x0D, 0x0A
    };

    ProtocolParser parser;
    uint8_t sortie[AML_PROTOCOL_MAX_FRAME_SIZE];
    size_t longueur_sortie = 0;
    int resultat;

    protocol_parser_init(&parser);

    /* Ajouter la première partie de la réponse. */
    resultat = protocol_parser_feed(
        &parser,
        premiere_partie,
        sizeof(premiere_partie)
    );

    assert(resultat == 0);

    /* La réponse est incomplète : aucune trame ne doit sortir. */
    resultat = protocol_parser_next(
        &parser,
        sortie,
        sizeof(sortie),
        &longueur_sortie
    );

    assert(resultat == 0);

    /* Ajouter les octets restants. */
    resultat = protocol_parser_feed(
        &parser,
        seconde_partie,
        sizeof(seconde_partie)
    );

    assert(resultat == 0);

    /* La réponse doit maintenant être disponible en entier. */
    resultat = protocol_parser_next(
        &parser,
        sortie,
        sizeof(sortie),
        &longueur_sortie
    );

    assert(resultat == 1);
    assert(longueur_sortie == sizeof(reponse_attendue));

    assert(memcmp(
        sortie,
        reponse_attendue,
        sizeof(reponse_attendue)
    ) == 0);

    /* Vérifier qu'il ne reste aucune trame en attente. */
    resultat = protocol_parser_next(
        &parser,
        sortie,
        sizeof(sortie),
        &longueur_sortie
    );

    assert(resultat == 0);

    printf("OK : reception d'une trame fragmentee\n\n");
}

/*
 * Test 4 : deux réponses reçues dans un seul bloc de données.
 *
 * Le parseur doit extraire la première réponse, puis la seconde,
 * sans perdre les octets qui suivent la première trame.
 */
static void test_deux_reponses(void)
{
    const uint8_t reponse[] = {
        0xFE, 0xEF, 0x0C, 0x09, 0xAB, 0xBB,
        0x04, 0xF0, 0x16, 0xE2, 0x55, 0x0D, 0x0A
    };

    uint8_t donnees[sizeof(reponse) * 2];
    uint8_t sortie[AML_PROTOCOL_MAX_FRAME_SIZE];

    ProtocolParser parser;
    size_t longueur_sortie = 0;

    /* Placer deux réponses consécutives dans le même bloc. */
    memcpy(donnees, reponse, sizeof(reponse));
    memcpy(donnees + sizeof(reponse), reponse, sizeof(reponse));

    protocol_parser_init(&parser);

    assert(protocol_parser_feed(
        &parser,
        donnees,
        sizeof(donnees)
    ) == 0);

    /* Extraire la première réponse. */
    assert(protocol_parser_next(
        &parser,
        sortie,
        sizeof(sortie),
        &longueur_sortie
    ) == 1);

    assert(longueur_sortie == sizeof(reponse));
    assert(memcmp(sortie, reponse, sizeof(reponse)) == 0);

    /* Extraire la deuxième réponse. */
    assert(protocol_parser_next(
        &parser,
        sortie,
        sizeof(sortie),
        &longueur_sortie
    ) == 1);

    assert(longueur_sortie == sizeof(reponse));
    assert(memcmp(sortie, reponse, sizeof(reponse)) == 0);

    /* Il ne doit plus rester de données exploitables. */
    assert(protocol_parser_next(
        &parser,
        sortie,
        sizeof(sortie),
        &longueur_sortie
    ) == 0);

    printf("OK : extraction de deux trames consecutives\n\n");
}

/*
 * Test 5 : présence d'octets parasites avant une réponse.
 *
 * Le parseur doit ignorer les octets précédant FE EF et retrouver
 * la réponse complète.
 */
static void test_bruit_avant_reponse(void)
{
    const uint8_t donnees[] = {
        0x00, 0x42, 0x99,
        0xFE, 0xEF, 0x0C, 0x09, 0xAB, 0xBB,
        0x04, 0xF0, 0x16, 0xE2, 0x55, 0x0D, 0x0A
    };

    const uint8_t reponse_attendue[] = {
        0xFE, 0xEF, 0x0C, 0x09, 0xAB, 0xBB,
        0x04, 0xF0, 0x16, 0xE2, 0x55, 0x0D, 0x0A
    };

    ProtocolParser parser;
    uint8_t sortie[AML_PROTOCOL_MAX_FRAME_SIZE];
    size_t longueur_sortie = 0;

    protocol_parser_init(&parser);

    assert(protocol_parser_feed(
        &parser,
        donnees,
        sizeof(donnees)
    ) == 0);

    assert(protocol_parser_next(
        &parser,
        sortie,
        sizeof(sortie),
        &longueur_sortie
    ) == 1);

    assert(longueur_sortie == sizeof(reponse_attendue));

    assert(memcmp(
        sortie,
        reponse_attendue,
        sizeof(reponse_attendue)
    ) == 0);

    printf("OK : recherche d'une trame apres du bruit\n\n");
}

/*
 * Point d'entrée des tests.
 *
 * Chaque test utilise assert() : si une vérification échoue,
 * le programme s'arrête et indique généralement la ligne concernée.
 */
int main(void)
{
    printf("=== Tests du protocole AML005 ===\n\n");

    /* Tests de construction des requêtes. */
    test_requete_statut();
    test_requete_parametres();

    /* Tests de réception et d'extraction des trames. */
    test_reponse_fragmentee();
    test_deux_reponses();
    test_bruit_avant_reponse();

    printf("Tous les tests sont passes.\n");

    return 0;
}