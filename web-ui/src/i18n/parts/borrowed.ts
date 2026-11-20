// Copy for screen C2, Borrowed element (wireframes v01 fr/Borrowed.html, en/Borrowed.html).
// {what} {who} {time}: no Rust call gives them yet (data-missing), the dialog fills them with "—".
export const fr = {
  "borrowed.title": "Élément emprunté",
  "borrowed.belongs": "{what} appartient à {who}",
  "borrowed.text": "{who} l'a emprunté à {time}. Vous pourrez le modifier quand elle aura synchronisé. Synchroniser maintenant récupère son travail.",
  "borrowed.cancel": "Annuler",
  "borrowed.sync": "Synchroniser",
  "borrowed.hint": "Clic : sélectionner · un élément que vous changez est emprunté pour vous",
} as const;
export const en: Record<keyof typeof fr, string> = {
  "borrowed.title": "Borrowed element",
  "borrowed.belongs": "{what} belongs to {who}",
  "borrowed.text": "{who} borrowed it at {time}. You can change it once she has synchronized. Synchronizing now gets her work.",
  "borrowed.cancel": "Cancel",
  "borrowed.sync": "Synchronize",
  "borrowed.hint": "Click: select · an element you change is borrowed for you",
};
