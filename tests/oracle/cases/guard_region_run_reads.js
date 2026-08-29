// THE READ-RUN MERGE of the guarded-region pass (src/lower/guard_region.h).
//
// `dot4` is the `Vector3.applyMatrix4` shape: every element is read and
// consumed before the next one is read. The pass tests the receiver once with
// `is.dense_array`, performs all four reads, and tests the four values
// afterwards — so a failed value test enters the slow copy in front of the
// FIRST read and the slow copy reads all four again.
//
// Reading a dense Array's element twice is not observable and reading a plain
// object's index twice is: `{ get "0"() {} }` is an accepted program. Every
// case below is about which of those two the receiver is, and about what the
// slow copy must therefore be allowed to redo.

function dot4(e, x, y, z) {
	return e[0] * x + e[1] * y + e[2] * z + e[3];
}

function show(label, value) {
	console.log(label + " " + value);
}

// 1. The shape the merge exists for: a dense Array of Numbers. Every test
//    holds and the whole run is loads.
show("dense", dot4([2, 3, 4, 5], 10, 100, 1000));

// 2. One member is a String. The receiver test holds, so the reads were
//    loads; the value test fails and the slow copy redoes the arithmetic with
//    ToNumber. Redoing the loads changes nothing, so the answer is case 1's.
show("coercible", dot4([2, "3", 4, 5], 10, 100, 1000));

// 3. The same with a `valueOf` that counts itself. It runs ONCE: the fast
//    copy's reads called nothing, and only the slow copy coerces.
var valueOfCalls = 0;
var countedThree = {
	valueOf: function () {
		valueOfCalls = valueOfCalls + 1;
		return 3;
	}
};
show("valueOf", dot4([2, countedThree, 4, 5], 10, 100, 1000));
show("valueOfCalls", valueOfCalls);

// 4. A PLAIN OBJECT with index-named accessors. `is.dense_array` refuses it,
//    the fast copy performs no read at all, and each getter runs exactly once
//    in source order. Four entries here and not eight is the whole point of
//    testing the receiver in front of the run rather than after it.
var getterLog = [];
var withGetters = {
	get "0"() {
		getterLog.push("g0");
		return 2;
	},
	get "1"() {
		getterLog.push("g1");
		return "3";
	},
	get "2"() {
		getterLog.push("g2");
		return 4;
	},
	get "3"() {
		getterLog.push("g3");
		return 5;
	}
};
show("getters", dot4(withGetters, 10, 100, 1000));
show("getterLog", getterLog.join(","));

// 5. A getter that MUTATES a member read later. The answer is the one the
//    reads see in source order, which only holds if no read was hoisted above
//    a read that runs user code.
var mutating = {
	third: 4,
	get "0"() {
		this.third = 40;
		return 2;
	},
	get "1"() {
		return 3;
	},
	get "2"() {
		return this.third;
	},
	get "3"() {
		return 5;
	}
};
show("mutating", dot4(mutating, 10, 100, 1000));

// 6. The LENGTH half of the receiver test: an Array too short for the run's
//    largest index. Reading past the end is `undefined` (10.4.2.1 over an
//    absent index) and ToNumber(undefined) is NaN (7.1.4.1).
show("short", dot4([2, 3], 10, 100, 1000));

// 7. A `valueOf` that mutates a LATER element of the Array it is in. The
//    coercion runs BETWEEN two reads in the original program, so the slow
//    copy — where a failed value test ends up — has to read that element
//    after the mutation and not before it.
var aliased = [
	{
		valueOf: function () {
			aliased[2] = 40;
			return 2;
		}
	},
	3,
	4,
	5
];
show("aliasing", dot4(aliased, 10, 100, 1000));

// 8. Not an object at all, so the receiver test's first half refuses it.
//    `undefined[0]` throws (6.2.5.5 over a nullish base), which is the one
//    thing a merged run must still do at the read it would have done it at.
try {
	dot4(undefined, 1, 1, 1);
} catch (e) {
	show("nullish", e instanceof TypeError);
}

// 9. A String receiver: an object test's other refusal. `"ab"[0]` is "a" and
//    "ab"[2] is undefined, so this is a coercion of two Strings and two
//    undefineds.
show("string", dot4("ab", 1, 1, 1));

// 10. The fast path again, after everything above has taught the inline
//     caches other shapes. The answer does not depend on what came before it.
show("dense again", dot4([2, 3, 4, 5], 10, 100, 1000));
