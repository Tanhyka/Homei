"""Move tree and branching variation structures for Cờ Lông Phượng (parity with ColongphuongGUI_1)."""

from __future__ import annotations

import copy
from typing import Any, Dict, List, Optional
import uuid

GLYPHS = ("!", "?", "!!", "??", "!?", "?!")


def _same_move_dict(a: Optional[dict], b: Optional[dict]) -> bool:
    """Checks if two move dictionaries represent the identical game action."""
    if a is None or b is None:
        return a == b
    return (
        a.get("kind") == b.get("kind")
        and a.get("src") == b.get("src")
        and a.get("dst") == b.get("dst")
        and a.get("ptype") == b.get("ptype")
        and a.get("facing") == b.get("facing")
        and (a.get("rotate_after", 0) % 4) == (b.get("rotate_after", 0) % 4)
    )


class MoveNode:
    """Node in the game move tree supporting branching variations (nước đi con / rẽ nhánh)."""

    def __init__(
        self,
        move: Optional[dict] = None,
        parent: Optional[MoveNode] = None,
        notation_text: str = "",
        side: str = "",
        ply: int = 0,
        state: Optional[dict] = None,
        node_id: Optional[str] = None,
    ):
        self.node_id: str = node_id or uuid.uuid4().hex
        self.move: Optional[dict] = copy.deepcopy(move) if move else None
        self.parent: Optional[MoveNode] = parent
        self.children: List[MoveNode] = []
        self.notation_text: str = notation_text
        self.side: str = side
        self.ply: int = ply
        self.state: Optional[dict] = copy.deepcopy(state) if state else None
        self.comment: str = ""
        self.glyphs: List[str] = []
        self.result: Optional[str] = None
        self.metadata: Dict[str, Any] = {}

    def is_root(self) -> bool:
        return self.parent is None

    def is_mainline(self) -> bool:
        """Returns True if this node belongs to the primary continuation (children[0])."""
        curr = self
        while curr.parent is not None:
            if curr.parent.children[0] is not curr:
                return False
            curr = curr.parent
        return True

    def variation_depth(self) -> int:
        """Counts how many branching points deviate from the mainline."""
        depth = 0
        curr = self
        while curr.parent is not None:
            if curr.parent.children[0] is not curr:
                depth += 1
            curr = curr.parent
        return depth

    def path_from_root(self) -> List[MoveNode]:
        """Returns the sequential path of nodes from root down to self."""
        path = []
        curr: Optional[MoveNode] = self
        while curr is not None:
            path.append(curr)
            curr = curr.parent
        path.reverse()
        return path

    def add_child(
        self,
        move: Optional[dict],
        state: Optional[dict],
        notation_text: str = "",
        side: str = "",
        ply: Optional[int] = None,
    ) -> MoveNode:
        """Appends a child move or returns existing child if move already exists."""
        for child in self.children:
            if _same_move_dict(child.move, move):
                if notation_text and not child.notation_text:
                    child.notation_text = notation_text
                if state and not child.state:
                    child.state = copy.deepcopy(state)
                return child

        actual_ply = ply if ply is not None else (self.ply + 1)
        new_node = MoveNode(
            move=move,
            parent=self,
            notation_text=notation_text,
            side=side,
            ply=actual_ply,
            state=state,
        )
        self.children.append(new_node)
        return new_node

    def promote(self) -> None:
        """Promotes this node to become the mainline continuation at all ancestor branch points."""
        curr = self
        while curr.parent is not None:
            parent = curr.parent
            if curr in parent.children:
                parent.children.remove(curr)
                parent.children.insert(0, curr)
            curr = parent

    def delete_branch(self) -> Optional[MoveNode]:
        """Detaches this node and its descendants from its parent. Returns parent node."""
        if self.parent is None:
            raise ValueError("Cannot delete root node")
        parent = self.parent
        if self in parent.children:
            parent.children.remove(self)
        self.parent = None
        return parent

    def all_nodes(self) -> List[MoveNode]:
        """Returns all nodes in the subtree rooted at self via pre-order traversal."""
        result = [self]
        for child in self.children:
            result.extend(child.all_nodes())
        return result
