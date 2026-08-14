// Class fields: public instance fields, static fields, and derived initialization order.

class Counter {
  count = 0;
  step = 1;
  uninit;
  static totalCreated = 0;

  constructor(initialStep) {
    if (initialStep !== undefined) {
      this.step = initialStep;
    }
    Counter.totalCreated = Counter.totalCreated + 1;
  }

  inc() {
    this.count = this.count + this.step;
    return this.count;
  }
}

console.log(Counter.totalCreated);
const c1 = new Counter();
console.log(c1.count);
console.log(c1.step);
console.log(c1.uninit);
console.log(c1.inc());
console.log(c1.inc());
console.log(Counter.totalCreated);

const c2 = new Counter(5);
console.log(c2.count);
console.log(c2.step);
console.log(c2.inc());
console.log(Counter.totalCreated);

// Inheritance: instance fields of derived classes run after super()
class Animal {
  kind = "animal";
  sound = "unknown";

  constructor(sound) {
    if (sound) this.sound = sound;
    console.log("Animal init:", this.kind, this.sound);
  }
}

class Dog extends Animal {
  breed = "canine";
  legs = 4;

  constructor(sound, breed) {
    super(sound);
    if (breed) this.breed = breed;
    console.log("Dog init:", this.kind, this.sound, this.breed, this.legs);
  }
}

const d = new Dog("woof", "retriever");
console.log(d.kind);
console.log(d.sound);
console.log(d.breed);
console.log(d.legs);
