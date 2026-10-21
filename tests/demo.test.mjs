import test from 'node:test';
import assert from 'node:assert/strict';
import { DemoTransport } from '../web/demo.mjs';
import { crc16, hex, unbase64 } from '../web/core.mjs';

test('actual demo history includes a valid Modbus request and response', async () => {
  let transport, timer;
  try {
    const records = await new Promise((resolve, reject) => {
      timer = setTimeout(() => reject(new Error('Demo history was not delivered')), 2000);
      transport = new DemoTransport({
        onOpen() { transport.send(JSON.stringify({ v: 1, id: 'check-history', type: 'history.get', payload: { after_seq: 0 } })); },
        onMessage(event) {
          const message = JSON.parse(event.data);
          if (message.type === 'history') resolve(message.payload.records);
        },
        onClose() {},
      });
    });
    const request = unbase64(records[0].data_b64);
    const response = unbase64(records[1].data_b64);
    assert.equal(records[0].direction, 'tx');
    assert.equal(records[1].direction, 'rx');
    assert.equal(hex(request), '01 03 00 00 00 02 C4 0B');
    assert.equal(hex(response), '01 03 04 00 7B 00 2D 4A 37');
    assert.equal(crc16(request), 0);
    assert.equal(crc16(response), 0);
  } finally {
    clearTimeout(timer);
    transport?.close();
  }
});
