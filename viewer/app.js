(function () {
  'use strict';

  var count = document.getElementById('count');
  var notesEl = document.getElementById('notes');
  var newerButton = document.getElementById('newer');
  var olderButton = document.getElementById('older');
  var pageLabel = document.getElementById('page');
  var doneButton = document.getElementById('done');

  function getReturnTo() {
    var result = /(?:^|&)return_to=([^&]*)/.exec(location.search.slice(1));
    return result ? decodeURIComponent(result[1]) : 'pebblejs://close#';
  }

  function closeWith(command) {
    location.href = getReturnTo() + encodeURIComponent(JSON.stringify(command));
  }

  function readPayload() {
    var fragment = location.hash;
    if (fragment.indexOf('#notes=') !== 0) {
      return { notes: [], total: 0, offset: 0, pageSize: 4 };
    }
    var data = JSON.parse(decodeURIComponent(fragment.slice(7)));
    // Remove notes from the visible address/history after parsing.
    try {
      history.replaceState(null, '', location.pathname + location.search);
    } catch (ignored) {}
    if (!data || !Array.isArray(data.notes)) {
      throw new Error('Malformed notes payload');
    }
    return data;
  }

  function formatTimestamp(value) {
    var date = new Date(value);
    if (isNaN(date.getTime())) return 'Unknown time';
    return date.toLocaleString(undefined, {
      year: 'numeric', month: 'short', day: 'numeric',
      hour: 'numeric', minute: '2-digit'
    });
  }

  var payload;
  try {
    payload = readPayload();
  } catch (error) {
    count.textContent = 'Could not read notes';
    notesEl.textContent = 'The notes link could not be decoded. Reopen Settings.';
    newerButton.disabled = true;
    olderButton.disabled = true;
    doneButton.addEventListener('click', function () { closeWith({ action: 'close' }); });
    return;
  }

  var offset = Number(payload.offset) || 0;
  var total = Number(payload.total) || 0;
  var pageSize = Number(payload.pageSize) || 4;
  count.textContent = payload.error ? payload.error :
    (total === 1 ? '1 saved note' : total + ' saved notes');
  pageLabel.textContent = total ?
    (offset + 1) + '\u2013' + Math.min(offset + pageSize, total) + ' of ' + total :
    'No notes';
  newerButton.disabled = offset <= 0;
  olderButton.disabled = offset + pageSize >= total;

  if (payload.notes.length === 0) {
    var empty = document.createElement('p');
    empty.className = 'empty';
    empty.textContent = 'Your dictated notes will appear here.';
    notesEl.appendChild(empty);
  }

  payload.notes.forEach(function (note) {
    var article = document.createElement('article');
    article.className = 'note';
    var date = document.createElement('time');
    date.textContent = formatTimestamp(note.createdAt);
    var text = document.createElement('p');
    // textContent ensures dictated content is never interpreted as HTML.
    text.textContent = note.text;
    var actions = document.createElement('div');
    actions.className = 'actions';
    var copy = document.createElement('button');
    copy.type = 'button';
    copy.textContent = 'Copy';
    copy.addEventListener('click', function () {
      if (!navigator.clipboard || !navigator.clipboard.writeText) {
        copy.textContent = 'Select note text';
        return;
      }
      navigator.clipboard.writeText(note.text).then(function () {
        copy.textContent = 'Copied';
      }, function () {
        copy.textContent = 'Select note text';
      });
    });
    var remove = document.createElement('button');
    remove.type = 'button';
    remove.className = 'danger';
    remove.textContent = 'Delete';
    remove.addEventListener('click', function () {
      if (window.confirm('Permanently delete this note?')) {
        closeWith({ action: 'delete', id: note.id, offset: offset });
      }
    });
    actions.appendChild(copy);
    actions.appendChild(remove);
    article.appendChild(date);
    article.appendChild(text);
    article.appendChild(actions);
    notesEl.appendChild(article);
  });

  newerButton.addEventListener('click', function () {
    closeWith({ action: 'page', offset: offset - pageSize });
  });
  olderButton.addEventListener('click', function () {
    closeWith({ action: 'page', offset: offset + pageSize });
  });
  doneButton.addEventListener('click', function () {
    closeWith({ action: 'close' });
  });
}());
