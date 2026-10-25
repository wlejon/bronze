// A private `Id` that is a number. Nothing outside this module can name it,
// and it does not decide what `Id` means anywhere else.
type Id = number;

export interface Account { id: Id; name: string }

export function nextId(last: Id): Id {
    let next: Id = last + 1;
    return next * 2;
}

export function makeAccount(name: string): Account {
    const id: Id = nextId(name.length);
    return { id, name };
}
