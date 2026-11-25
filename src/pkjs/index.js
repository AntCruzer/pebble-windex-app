/*
 * PebbleKit JS phone component, running inside the Pebble mobile application.
 * Storage here is NOT the same origin/storage as the hosted configuration page.
 * The viewer receives a short, read-only page of notes via the URL fragment.
 */
(function () {
  'use strict';

  var STORAGE_KEY = 'voice_notes_v1';
  var PAGE_SIZE = 4;
  // REQUIRED before using the phone's Settings/gear icon:
  // Deploy viewer/ on GitHub Pages, and set this to its public HTTPS URL.
  var CONFIG_URL = 'https://antcruzer.github.io/pebble-windex-app/viewer/';

  // PebbleJS C and JS keys are defined by their order in package.json.
  var NUMERIC_KEYS = { NoteId: 0, NoteText: 1 };

  function payloadValue(payload, key) {
    return payload[key] !== undefined
      ? payload[key]
      : payload[String(NUMERIC_KEYS[key])];
  }

  function readNotes() {
    var stored = localStorage.getItem(STORAGE_KEY);
    if (!stored) {
      return [];
    }
    var parsed = JSON.parse(stored);
    if (!Array.isArray(parsed)) {
      throw new Error('Invalid saved notes format');
    }
    return parsed;
  }

  function writeNotes(notes) {
    var serialized = JSON.stringify(notes);
    localStorage.setItem(STORAGE_KEY, serialized);
    // Do not acknowledge success unless persistent storage can be read back.
    if (localStorage.getItem(STORAGE_KEY) !== serialized) {
      throw new Error('Stored notes failed verification');
    }
  }

  function saveNote(id, text) {
    var notes = readNotes();
    for (var i = 0; i < notes.length; i++) {
      if (notes[i].id === id) {
        if (notes[i].text !== text) {
          throw new Error('Duplicate NoteId with different content');
        }
        return; // Same note sent again because a transport ACK was lost.
      }
    }
    notes.unshift({
      id: id,
      text: text,
      createdAt: new Date().toISOString()
    });
    writeNotes(notes);
  }

  function sendSaveAck(id, success) {
    Pebble.sendAppMessage({ AckId: id, AckStatus: success ? 1 : 0 },
      function () {
        console.log('Phone confirmed note ' + id + ': ' + success);
      },
      function (error) {
        // The user can retry on the watch; duplicate messages are harmless.
        console.log('Could not deliver save acknowledgement: ' +
                    JSON.stringify(error));
      });
  }

  function clampOffset(offset, total) {
    var candidate = Number(offset);
    if (!isFinite(candidate) || candidate < 0) {
      candidate = 0;
    }
    candidate = Math.floor(candidate / PAGE_SIZE) * PAGE_SIZE;
    var last = Math.floor(Math.max(0, total - 1) / PAGE_SIZE) * PAGE_SIZE;
    return Math.min(candidate, last);
  }

  function openViewer(requestedOffset) {
    var notes;

    var viewerError = null;
    try {
      notes = readNotes();
    } catch (error) {
      // Show a descriptive error without overwriting potentially recoverable data.
      console.log('Cannot display notes: ' + error);
      viewerError = 'Saved notes could not be read. No data was changed.';
      notes = [];
    }
    var offset = clampOffset(requestedOffset, notes.length);
    var payload = {
      offset: offset,
      total: notes.length,
      pageSize: PAGE_SIZE,
      error: viewerError,
      notes: notes.slice(offset, offset + PAGE_SIZE)
    };
    var base = CONFIG_URL.split('#')[0];
    var target = base + '#notes=' + encodeURIComponent(JSON.stringify(payload));
    Pebble.openURL(target);
  }

  function removeNote(id) {
    var notes = readNotes();
    var kept = [];
    for (var i = 0; i < notes.length; i++) {
      if (notes[i].id !== id) {
        kept.push(notes[i]);
      }
    }
    if (kept.length !== notes.length) {
      writeNotes(kept);
    }
  }

  Pebble.addEventListener('ready', function () {
    console.log('Voice Notes PebbleKit JS is ready');
    Pebble.sendAppMessage({ PhoneReady: 1 },
      function () {},
      function (error) { console.log('Ready handshake failed: ' + JSON.stringify(error)); });
  });

  Pebble.addEventListener('appmessage', function (event) {
    var payload = event.payload || {};
    var rawId = payloadValue(payload, 'NoteId');
    var text = payloadValue(payload, 'NoteText');
    if (rawId === undefined && text === undefined) {
      return; // Ignore other messages.
    }

    var id = Number(rawId);
    var ok = false;
    try {
      if (id > 0 && isFinite(id) && Math.floor(id) === id &&
          typeof text === 'string' && text.length > 0) {
        saveNote(id, text);
        ok = true;
      } else {
        console.log('Ignoring invalid note payload');
      }
    } catch (error) {
      console.log('Local storage failed: ' + error);
    }
    if (id > 0 && isFinite(id)) {
      sendSaveAck(id, ok);
    }
  });

  Pebble.addEventListener('showConfiguration', function () {
    openViewer(0);
  });

  Pebble.addEventListener('webviewclosed', function (event) {
    if (!event || !event.response || event.response === 'CANCELLED') {
      return;
    }
    var command;
    try {
      command = JSON.parse(decodeURIComponent(event.response));
    } catch (error) {
      console.log('Unrecognized viewer action: ' + error);
      return;
    }
    if (command.action === 'page') {
      openViewer(command.offset);
    } else if (command.action === 'delete' &&
               typeof command.id === 'number') {
      try {
        removeNote(command.id);
      } catch (error) {
        console.log('Could not delete note: ' + error);
      }
      openViewer(command.offset);
    }
  });

  // Test-only exports. PebbleKit JS itself does not have Node's `module`.
  if (typeof module !== 'undefined' && module.exports) {
    module.exports = {
      readNotes: readNotes,
      saveNote: saveNote,
      removeNote: removeNote,
      clampOffset: clampOffset,
      openViewer: openViewer
    };
  }
}());
