import { loadLazy } from './importer.js';

// Nested a few functions deep and inside a switch, which is where the three.js
// editor's own dynamic imports live: a callback registered by a method of a
// factory function, capturing the names around it.
export function makeRunner(tag) {

    function dispatch(kind, onDone) {

        switch (kind) {

            case 'lazy':
            {
                const label = tag + ':' + kind;
                loadLazy(label).then(function (text) { onDone(text); });
                break;
            }

            default:
                onDone('unknown');
                break;

        }

    }

    return dispatch;

}
