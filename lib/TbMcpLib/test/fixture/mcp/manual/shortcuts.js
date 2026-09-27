const keys = {
    'Alt': 'Alt',
    'Ctrl': 'Ctrl',
    '\'': '\'',
    '\\': '\\',
    'Space': 'Space',
};
const menu = {
    'Menu/View/Toggle Info Panel': { path: ['View', 'Toggle Info Panel'], shortcut: [ { key: '4', modifiers: ['Ctrl', ] } ] },
    'Menu/View/Maximize Current View': { path: ['View', 'Maximize Current View'], shortcut: [ { key: 'Space', modifiers: ['Ctrl', ] } ] },
    'Menu/View/Camera/Move to Next Point': { path: ['View', 'Camera', 'Move Camera to Next Point'], shortcut: [ { key: '', modifiers: [] } ] },
    'Menu/Edit/Copy': { path: ['Edit', 'Copy'], shortcut: [ { key: 'C', modifiers: ['Ctrl', ] } ] },
    'Menu/Edit/Tools/Vertex Tool': { path: ['Tools', 'Vertex Tool'], shortcut: [ { key: 'V', modifiers: [] } ] },
};
const actions = {
    'Controls/Camera/Move forward': [ { key: 'W', modifiers: [] } ],
    'Menu/Edit/Tools/Vertex Tool': [ { key: 'V', modifiers: [] } ],
};
