// A private `Id` of another shape, a string, with the same name as ids_a's.
// Were that module's `Id` visible here, every annotation below would claim a
// number of a value that is a string.
type Id = string;
export type Tag = `tag-${string}`;

export class Label {
    constructor(public id: Id) {}
    tag(): Tag {
        const joined: Id = this.id + "-" + this.id.length;
        return `tag-${joined}`;
    }
}

export function concatIds(a: Id, b: Id): Id {
    const both: Id = a + b;
    return both;
}
