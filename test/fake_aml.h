#ifndef FAKE_AML_H
#define FAKE_AML_H

/*
 * test/fake_aml.h — Faux AML005 pour tester sans Bluetooth
 *
 * fake_aml.c fournit ses propres aml_connect, aml_send, aml_receive,
 * aml_flush, aml_disconnect, config_load et config_select : un test lié
 * avec fake_aml.o (et SANS transport.o ni config.o) parle donc à ce faux
 * appareil au lieu du vrai.
 *
 * Le test prépare les réponses à l'avance (fake_push_response), puis
 * vérifie ce qui a été envoyé (fake_sent). L'horloge est elle aussi
 * pilotée par le test (fake_now) : aucune attente réelle.
 */

#include <stdint.h>
#include <stddef.h>

#include "core/sweetdawn.h"

#define FAKE_QUEUE_MAX 16

typedef struct {
    uint8_t data[AML_PROTOCOL_MAX_FRAME_SIZE];
    size_t length;
    bool spontaneous;   /* message spontané, déjà « arrivé » */
} FakeFrame;

/* Comportement du faux appareil (remis à zéro par fake_reset). */
extern int fake_connect_error;   /* code renvoyé par aml_connect          */
extern int fake_send_error;      /* code renvoyé par aml_send             */
extern int fake_empty_code;      /* aml_receive sans réponse en attente   */

/* Ce qui s'est passé. */
extern int fake_connect_calls;
extern int fake_disconnect_calls;
extern int fake_send_calls;
extern uint8_t fake_last_request_code;
extern FakeFrame fake_sent[FAKE_QUEUE_MAX];   /* trames envoyées, dans l'ordre */
extern int fake_sent_count;

/* Horloge du test, en millisecondes. */
extern int64_t fake_now;

/* Oublie les réponses en attente, les trames envoyées et les compteurs. */
void fake_reset(void);

/*
 * Ajoute à la file une réponse de l'AML portant ce code et ces données ;
 * aml_receive la rendra au prochain appel.
 */
void fake_push_response(uint8_t code, const uint8_t *args, size_t args_length);

/*
 * Ajoute un message spontané de l'AML (alarme, éclairage changé...).
 *
 * Différence avec une réponse : une réponse n'« arrive » qu'après une
 * requête, alors qu'un message spontané est lisible tout de suite,
 * y compris par une lecture sans attente (aml_receive avec 0 ms).
 */
void fake_push_event(uint8_t code, const uint8_t *args, size_t args_length);

/*
 * Prépare app comme après sweetdawn_init (sans fichier de configuration),
 * avec l'horloge du test, puis appelle fake_reset.
 */
void fake_setup(SweetDawn *app);

#endif /* FAKE_AML_H */
