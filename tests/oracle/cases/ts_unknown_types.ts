// Annotations naming types bronze has no declaration for: the DOM's, the
// standard library's, and names an ambient declaration or a .d.ts file would
// supply. Each is a hint bronze cannot read, so it hints nothing; none fails
// the build, and the program runs as its values say.
declare const hostApi: HostApi | undefined;
declare function hostCall(name: string): HostResult;
declare namespace Ambient {
    interface Options { verbose: boolean }
}

let el: HTMLElement | null = null;
let canvas: HTMLCanvasElement | undefined;
const listeners: Map<string, (e: KeyboardEvent) => void> = new Map();

function onKey(e: KeyboardEvent, w: Window & typeof globalThis): string {
    return `${e.key}:${typeof w}`;
}

function describe(target: EventTarget | Element | Document, opts?: Ambient.Options): string {
    return `${typeof target} ${opts?.verbose ?? false}`;
}

async function load(url: RequestInfo | URL): Promise<Response | undefined> {
    return url === "none" ? undefined : undefined;
}

class Widget implements EventListenerObject {
    node: HTMLDivElement | null = null;
    handleEvent(evt: Event): void {
        console.log("handled", (evt as CustomEvent<number>).detail);
    }
}

// Names from libraries that are not loaded, generic or qualified.
let promised: Promise<Foo.Bar<number>> | null = null;
let record: Record<keyof DocumentEventMap, number> | undefined;
let partial: Partial<CSSStyleDeclaration> = { color: "red" } as any;
let fromLib: lib.nested.Thing<string, Widget>[] = [];

listeners.set("down", (e) => console.log("down", e.key));
listeners.get("down")!({ key: "x" } as KeyboardEvent);
console.log(onKey({ key: "Enter" } as unknown as KeyboardEvent, globalThis as any));
console.log(describe({} as Document, { verbose: true }), describe({} as Element));
console.log(el, canvas, promised, record, partial.color, fromLib.length);
console.log(typeof hostApi === "undefined" ? "no host" : "host");
new Widget().handleEvent({ detail: 7 } as unknown as Event);
load("none").then((r) => console.log("loaded", r));
