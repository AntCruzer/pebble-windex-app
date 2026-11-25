#include <pebble.h>
#include <stdint.h>
#include <string.h>

// Short notes only: Dictation allocates an internal buffer of this size.
#define NOTE_BUFFER_SIZE 512
#define ACK_TIMEOUT_MS 9000
#define CONNECT_DELAY_MS 1500
#define SUCCESS_SCREEN_MS 1400
#define PERSIST_KEY_SEQUENCE 1

typedef enum {
  STATE_STARTING,
  STATE_DICTATING,
  STATE_WAITING_PHONE,
  STATE_SAVING,
  STATE_DICTATION_ERROR,
  STATE_SAVE_UNCONFIRMED,
  STATE_SAVE_REJECTED,
  STATE_SAVED
} NoteState;

static Window *s_window;
static TextLayer *s_heading_layer;
static TextLayer *s_status_layer;
static TextLayer *s_hint_layer;
static DictationSession *s_dictation_session;
static AppTimer *s_timer;

static NoteState s_state = STATE_STARTING;
static char s_note_text[NOTE_BUFFER_SIZE];
static int32_t s_note_id;
static bool s_phone_ready;

static void prv_cancel_timer(void) {
  if (s_timer) {
    app_timer_cancel(s_timer);
    s_timer = NULL;
  }
}

static void prv_show(const char *status, const char *hint) {
  text_layer_set_text(s_status_layer, status);
  text_layer_set_text(s_hint_layer, hint);
}

static void prv_exit_app(void *context) {
  (void)context;
  s_timer = NULL;
  window_stack_pop_all(true);
}

static void prv_save_unconfirmed(const char *why) {
  prv_cancel_timer();
  s_state = STATE_SAVE_UNCONFIRMED;
  prv_show("NOT CONFIRMED", why);
}

static void prv_ack_timeout(void *context) {
  (void)context;
  s_timer = NULL;
  if (s_state == STATE_SAVING) {
    // No ACK isn't proof of failure. Retry uses the same ID; phone dedupes.
    prv_save_unconfirmed("Select: retry save\nBack: exit");
  }
}

static void prv_attempt_send(void) {
  if (s_note_id <= 0 || !s_note_text[0]) {
    prv_save_unconfirmed("No note to send");
    return;
  }

  prv_cancel_timer();
  s_state = STATE_SAVING;
  prv_show("SAVING...", "Waiting for phone");

  DictionaryIterator *out = NULL;
  AppMessageResult result = app_message_outbox_begin(&out);
  if (result != APP_MSG_OK || !out) {
    prv_save_unconfirmed("Select: retry save\nBack: exit");
    return;
  }

  if (dict_write_int32(out, MESSAGE_KEY_NoteId, s_note_id) != DICT_OK ||
      dict_write_cstring(out, MESSAGE_KEY_NoteText, s_note_text) != DICT_OK) {
    prv_save_unconfirmed("Select: retry save\nBack: exit");
    return;
  }

  result = app_message_outbox_send();
  if (result != APP_MSG_OK) {
    prv_save_unconfirmed("Select: retry save\nBack: exit");
    return;
  }

  // AppMessage's outbox-sent callback does NOT mean localStorage succeeded.
  s_timer = app_timer_register(ACK_TIMEOUT_MS, prv_ack_timeout, NULL);
}

static void prv_send_after_delay(void *context) {
  (void)context;
  s_timer = NULL;
  if (s_state == STATE_WAITING_PHONE) {
    // Fallback if a PhoneReady handshake was lost.
    prv_attempt_send();
  }
}

static int32_t prv_allocate_note_id(void) {
  int32_t last = persist_exists(PERSIST_KEY_SEQUENCE)
                     ? persist_read_int(PERSIST_KEY_SEQUENCE)
                     : 0;
  int32_t next = (last > 0 && last < INT32_MAX) ? last + 1 : 1;
  if (persist_write_int(PERSIST_KEY_SEQUENCE, next) < 0) {
    return 0;
  }
  return next;
}

static void prv_dictation_callback(DictationSession *session,
                                   DictationSessionStatus status,
                                   char *transcription,
                                   void *context) {
  (void)session;
  (void)context;
  if (s_state != STATE_DICTATING) {
    return;
  }

  if (status == DictationSessionStatusSuccess && transcription &&
      transcription[0] != '\0') {
    // The SDK-owned transcription pointer is invalid after this callback.
    strncpy(s_note_text, transcription, sizeof(s_note_text) - 1);
    s_note_text[sizeof(s_note_text) - 1] = '\0';
    s_note_id = prv_allocate_note_id();
    if (s_note_id == 0) {
      s_state = STATE_SAVE_REJECTED;
      prv_show("STORAGE ERROR", "Could not create note ID\nBack: exit");
      return;
    }

    if (s_phone_ready) {
      prv_attempt_send();
    } else {
      s_state = STATE_WAITING_PHONE;
      prv_show("CONNECTING...", "Preparing to save note");
      s_timer = app_timer_register(CONNECT_DELAY_MS,
                                   prv_send_after_delay, NULL);
    }
    return;
  }

  // A deliberate Back action in the system dictation UI cancels the note.
  if (status == DictationSessionStatusFailureTranscriptionRejected) {
    prv_exit_app(NULL);
    return;
  }

  s_state = STATE_DICTATION_ERROR;
  switch (status) {
    case DictationSessionStatusFailureNoSpeechDetected:
      prv_show("NO SPEECH", "Select: try again\nBack: exit");
      break;
    case DictationSessionStatusFailureConnectivityError:
      prv_show("PHONE OFFLINE", "Connect phone, then\nSelect: try again");
      break;
    case DictationSessionStatusFailureDisabled:
      prv_show("DICTATION OFF", "Enable dictation on phone\nBack: exit");
      break;
    default:
      prv_show("VOICE ERROR", "Select: try again\nBack: exit");
      break;
  }
}

static void prv_start_dictation(void *context) {
  (void)context;
  s_timer = NULL;
  if (s_state == STATE_SAVING || s_state == STATE_SAVED) {
    return;
  }

  if (!s_dictation_session) {
    s_dictation_session = dictation_session_create(
        NOTE_BUFFER_SIZE, prv_dictation_callback, NULL);
    if (!s_dictation_session) {
      s_state = STATE_DICTATION_ERROR;
      prv_show("VOICE UNAVAILABLE", "Check phone connection\nSelect: retry");
      return;
    }
    // Direct, one-interaction flow. System UI still handles recording.
    dictation_session_enable_confirmation(s_dictation_session, false);
    dictation_session_enable_error_dialogs(s_dictation_session, true);
  }

  s_note_text[0] = '\0';
  s_note_id = 0;
  s_state = STATE_DICTATING;
  prv_show("LISTENING...", "Speak your short note");
  // Pebble's API returns a DictationSessionStatus, not a boolean.
  // DictationSessionStatusSuccess is zero, so do not negate the result.
  if (dictation_session_start(s_dictation_session) != DictationSessionStatusSuccess) {
    s_state = STATE_DICTATION_ERROR;
    prv_show("VOICE UNAVAILABLE", "Select: try again\nBack: exit");
  }
}

static void prv_select_click_handler(ClickRecognizerRef recognizer,
                                      void *context) {
  (void)recognizer;
  (void)context;
  if (s_state == STATE_DICTATION_ERROR) {
    prv_start_dictation(NULL);
  } else if (s_state == STATE_SAVE_UNCONFIRMED ||
             s_state == STATE_SAVE_REJECTED) {
    if (s_note_id > 0) {
      // Retry same NoteId so localStorage never receives a duplicate entry.
      prv_attempt_send();
    } else {
      prv_start_dictation(NULL);
    }
  }
}

static void prv_click_config_provider(void *context) {
  (void)context;
  window_single_click_subscribe(BUTTON_ID_SELECT, prv_select_click_handler);
  // Back uses Pebble's standard app exit; in-flight sends cannot be unsent.
}

static void prv_inbox_received_handler(DictionaryIterator *iterator,
                                        void *context) {
  (void)context;
  Tuple *ready = dict_find(iterator, MESSAGE_KEY_PhoneReady);
  if (ready && ready->value->int32 == 1) {
    s_phone_ready = true;
    if (s_state == STATE_WAITING_PHONE) {
      prv_attempt_send();
    }
  }

  Tuple *ack_id = dict_find(iterator, MESSAGE_KEY_AckId);
  Tuple *ack_status = dict_find(iterator, MESSAGE_KEY_AckStatus);
  if (!ack_id || !ack_status || ack_id->value->int32 != s_note_id ||
      (s_state != STATE_SAVING &&
       s_state != STATE_SAVE_UNCONFIRMED &&
       s_state != STATE_SAVE_REJECTED)) {
    return;
  }

  prv_cancel_timer();
  if (ack_status->value->int32 == 1) {
    s_state = STATE_SAVED;
    prv_show("NOTE SAVED", "Returning to watchface");
    vibes_short_pulse();
    s_timer = app_timer_register(SUCCESS_SCREEN_MS, prv_exit_app, NULL);
  } else {
    s_state = STATE_SAVE_REJECTED;
    prv_show("SAVE FAILED", "Select: retry save\nBack: exit");
  }
}

static void prv_outbox_failed_handler(DictionaryIterator *iterator,
                                       AppMessageResult reason,
                                       void *context) {
  (void)iterator;
  (void)context;
  APP_LOG(APP_LOG_LEVEL_WARNING, "Note transport failed: %d", (int)reason);
  if (s_state == STATE_SAVING) {
    prv_save_unconfirmed("Select: retry save\nBack: exit");
  }
}

static void prv_window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);
  const int16_t width = bounds.size.w;

  s_heading_layer = text_layer_create(GRect(8, 24, width - 16, 34));
  text_layer_set_text(s_heading_layer, "VOICE NOTES");
  text_layer_set_font(s_heading_layer,
                      fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
  text_layer_set_text_alignment(s_heading_layer, GTextAlignmentCenter);
  text_layer_set_background_color(s_heading_layer, GColorBlack);
  text_layer_set_text_color(s_heading_layer, GColorWhite);
  layer_add_child(root, text_layer_get_layer(s_heading_layer));

  s_status_layer = text_layer_create(GRect(8, 76, width - 16, 57));
  text_layer_set_font(s_status_layer,
                      fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD));
  text_layer_set_text_alignment(s_status_layer, GTextAlignmentCenter);
  text_layer_set_background_color(s_status_layer, GColorBlack);
  text_layer_set_text_color(s_status_layer, GColorWhite);
  layer_add_child(root, text_layer_get_layer(s_status_layer));

  s_hint_layer = text_layer_create(GRect(12, 156, width - 24, 62));
  text_layer_set_font(s_hint_layer,
                      fonts_get_system_font(FONT_KEY_GOTHIC_18));
  text_layer_set_text_alignment(s_hint_layer, GTextAlignmentCenter);
  text_layer_set_background_color(s_hint_layer, GColorBlack);
  text_layer_set_text_color(s_hint_layer, GColorWhite);
  layer_add_child(root, text_layer_get_layer(s_hint_layer));

  prv_show("STARTING...", "Opening dictation");
}

static void prv_window_unload(Window *window) {
  (void)window;
  text_layer_destroy(s_heading_layer);
  text_layer_destroy(s_status_layer);
  text_layer_destroy(s_hint_layer);
}

static void prv_init(void) {
  s_window = window_create();
  window_set_background_color(s_window, GColorBlack);
  window_set_window_handlers(s_window, (WindowHandlers){
    .load = prv_window_load,
    .unload = prv_window_unload
  });
  window_set_click_config_provider(s_window, prv_click_config_provider);

  app_message_register_inbox_received(prv_inbox_received_handler);
  app_message_register_outbox_failed(prv_outbox_failed_handler);
  AppMessageResult result = app_message_open(128, 1024);
  if (result != APP_MSG_OK) {
    APP_LOG(APP_LOG_LEVEL_WARNING, "AppMessage init error: %d", (int)result);
  }

  window_stack_push(s_window, true);
  s_timer = app_timer_register(250, prv_start_dictation, NULL);
}

static void prv_deinit(void) {
  prv_cancel_timer();
  if (s_dictation_session) {
    dictation_session_destroy(s_dictation_session);
  }
  window_destroy(s_window);
}

int main(void) {
  prv_init();
  app_event_loop();
  prv_deinit();
}
