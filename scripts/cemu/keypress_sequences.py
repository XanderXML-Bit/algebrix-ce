from __future__ import annotations

from dataclasses import dataclass
from typing import Iterable, Sequence


@dataclass(frozen=True)
class KeyAction:
    kind: str
    key: str | None = None
    duration_ms: int = 0
    note: str = ""


TI84_CE_KEYS: dict[str, str] = {
    "ZERO": "0",
    "ONE": "1",
    "TWO": "2",
    "THREE": "3",
    "FOUR": "4",
    "FIVE": "5",
    "SIX": "6",
    "SEVEN": "7",
    "EIGHT": "8",
    "NINE": "9",
    "PLUS": "+",
    "MINUS": "-",
    "MULTIPLY": "*",
    "DIVIDE": "/",
    "POWER": "^",
    "DECIMAL": ".",
    "LEFT_PAREN": "(",
    "RIGHT_PAREN": ")",
    "NEGATE": "(-)",
    "ENTER": "enter",
    "CLEAR": "clear",
    "MODE": "mode",
    "YEQU": "yequ",
    "WINDOW": "window",
    "ZOOM": "zoom",
    "TRACE": "trace",
    "GRAPH": "graph",
    "SECOND": "2nd",
    "ALPHA": "alpha",
    "MATH": "math",
    "APPS": "apps",
    "PRGM": "prgm",
    "SIN": "sin",
    "COS": "cos",
    "TAN": "tan",
    "LN": "ln",
    "LOG": "log",
    "SQUARE": "^2",
    "RECIPROCAL": "-1",
    "X": "xton",
    "LEFT": "left",
    "RIGHT": "right",
    "UP": "up",
    "DOWN": "down",
    "DELETE": "del",
    "COMMA": ",",
    "STORE": "sto",
}


KEY_ALIASES: dict[str, str] = {
    **{key.lower(): value for key, value in TI84_CE_KEYS.items()},
    **{value.lower(): value for value in TI84_CE_KEYS.values()},
    "add": "+",
    "subtract": "-",
    "sub": "-",
    "times": "*",
    "multiply": "*",
    "mul": "*",
    "divide": "/",
    "div": "/",
    "point": ".",
    "dot": ".",
    "return": "enter",
    "del": "del",
    "sto": "sto",
    "sto>": "sto",
    "store": "sto",
    "storeto": "sto",
    "y=": "yequ",
}

# TI-84 Plus CE ALPHA layer exactly as printed on the keycaps (row order:
# math..÷ = A..M, log..2 = N..Z). Must match src/core/launcher/main.c.
ALPHA_KEY_MAP: dict[str, str] = {
    "a": "math",
    "b": "apps",
    "c": "prgm",
    "d": "-1",
    "e": "sin",
    "f": "cos",
    "g": "tan",
    "h": "^",
    "i": "^2",
    "j": ",",
    "k": "(",
    "l": ")",
    "m": "/",
    "n": "log",
    "o": "7",
    "p": "8",
    "q": "9",
    "r": "*",
    "s": "ln",
    "t": "4",
    "u": "5",
    "v": "6",
    "w": "-",
    "x": "sto",
    "y": "1",
    "z": "2",
}

EXPRESSION_KEY_MAP: dict[str, str] = {
    "+": "+",
    "-": "-",
    "*": "*",
    "/": "/",
    "^": "^",
    ".": ".",
    "(": "(",
    ")": ")",
    ",": ",",
    ";": "apps",
}


def normalize_key(key: str) -> str:
    normalized = KEY_ALIASES.get(key.lower())
    if normalized is None:
        raise KeyError(f"Unsupported TI-84 CE key alias: {key}")
    return normalized


def press(key: str, duration_ms: int = 0, note: str = "") -> KeyAction:
    return KeyAction(kind="key", key=normalize_key(key), duration_ms=duration_ms, note=note)


def pause(duration_ms: int, note: str = "") -> KeyAction:
    if duration_ms <= 0:
        raise ValueError("Pause duration must be positive.")
    return KeyAction(kind="pause", duration_ms=duration_ms, note=note)


def digits(number_text: str) -> list[KeyAction]:
    sequence: list[KeyAction] = []
    for character in number_text:
        if character.isdigit():
            sequence.append(press(character))
        elif character == ".":
            sequence.append(press("decimal"))
        elif character == "-":
            sequence.append(press("negate"))
        else:
            raise ValueError(f"Unsupported numeric character: {character}")
    return sequence


def looks_like_scientific_notation(expression_text: str, index: int) -> bool:
    if expression_text[index].lower() != "e":
        return False
    previous = expression_text[index - 1] if index > 0 else ""
    following = expression_text[index + 1] if index + 1 < len(expression_text) else ""
    return (previous.isdigit() or previous == ".") and (following.isdigit() or following in "+-")


def expression_tokens(expression_text: str) -> tuple[str, ...]:
    tokens: list[str] = []
    for index, character in enumerate(expression_text):
        if character.isspace():
            continue
        if character.isdigit():
            tokens.append(character)
            continue
        mapped = EXPRESSION_KEY_MAP.get(character)
        if mapped is not None:
            tokens.append(mapped)
            continue
        if character.isalpha():
            if looks_like_scientific_notation(expression_text, index):
                tokens.extend(("2nd", "(-)"))
                continue
            alpha_key = ALPHA_KEY_MAP.get(character.lower())
            if alpha_key is None:
                raise ValueError(f"Unsupported expression character: {character}")
            tokens.extend(("alpha", alpha_key))
            continue
        raise ValueError(f"Unsupported expression character: {character}")
    return tuple(tokens)


def token_sequence(token_values: Sequence[str]) -> list[KeyAction]:
    return [press(token) for token in token_values]


def expression(expression_text: str) -> list[KeyAction]:
    return token_sequence(expression_tokens(expression_text))


def evaluate_expression(expression_text: str, result_delay_ms: int = 2000) -> list[KeyAction]:
    return [
        *expression(expression_text),
        pause(200, note="settle before enter"),
        press("enter"),
        pause(result_delay_ms, note="wait for result"),
    ]


def boot_to_calcengine_prompt(launch_delay_ms: int = 4000) -> list[KeyAction]:
    return [
        pause(launch_delay_ms, note="wait for launch"),
        press("clear"),
        pause(500, note="clear splash to prompt"),
    ]


def open_program_menu(menu_delay_ms: int = 1200) -> list[KeyAction]:
    return [press("prgm"), pause(menu_delay_ms, note="wait for PRGM menu")]


def launch_program_from_menu(cursor_moves: int = 0, menu_delay_ms: int = 1200) -> list[KeyAction]:
    sequence = open_program_menu(menu_delay_ms=menu_delay_ms)
    for _ in range(cursor_moves):
        sequence.append(press("down"))
        sequence.append(pause(250, note="menu navigation"))
    sequence.append(press("enter"))
    sequence.append(pause(4000, note="wait for selected program launch"))
    return sequence


def dismiss_result_to_prompt(prompt_delay_ms: int = 500) -> list[KeyAction]:
    return [press("clear"), pause(prompt_delay_ms, note="return to prompt")]


def flatten(*parts: Iterable[KeyAction]) -> list[KeyAction]:
    flattened: list[KeyAction] = []
    for part in parts:
        flattened.extend(part)
    return flattened
