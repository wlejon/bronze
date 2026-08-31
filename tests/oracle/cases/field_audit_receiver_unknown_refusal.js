// The computed write whose receiver the pass cannot type at all.
//
// `field_audit_receiver_scoped_refusal.js` pins what a KNOWN receiver limits
// the damage to. This is the other end: a receiver read out of an element, so
// inference answers `Dynamic` for it and there is no class the write can be
// said to reach. The audit's only sound answer is the one it always had —
// refuse every name in the program — and the point of this case is that the
// ANSWERS stay right while the claims go away.
//
// It is a separate file from the scoped one on purpose. A program-wide refusal
// stands every name in its file down, so a scoped refusal and a program-wide
// one cannot be told apart if they share a program.

function show(label, v) {
  console.log(label + "=" + v + " (" + typeof v + ")");
}

class P {
  constructor() {
    this.x = 0;
    this.y = 0;
  }
  sum() {
    let s = this.x + this.y;
    for (let i = 0; i < 2; i++) s = this.x + this.y;
    return s;
  }
}

const kk = ["x"][0];

// `nest[0]` is an element read, which the flow pass answers `Dynamic` for, and
// a property read off a dynamic base is dynamic too. Nothing about this
// receiver names a class, and the write really does reach a P.
const nest = [{ inner: new P() }];
nest[0].inner[kk] = "hi";

const other = new P();
other.x = 7;
other.y = 8;

show("reached", nest[0].inner.sum());
show("untouched", other.sum());
