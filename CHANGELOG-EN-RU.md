# Candidate changes

- Restore per-dictionary result identity with dic_id while retaining SQLite v5 migration and NOCASE indexes.
- Restore UK/US flag buttons and Anki flag media with newly drawn flag SVGs; restore the assistant icon from the preserved updates source.
- Restore Russian language selection and translation compilation.
- Retain staging's English final-e fallback fix and Sentence Assistant prompt tests.
- Add regression coverage for database migration, dictionary separation, case variants, missing words and disabled dictionaries.
- Wait up to five seconds for transient SQLite database locks and include database path/SQLite diagnostics when database open or preparation fails.
- Generate a stable clean-profile launcher for each staged Windows runtime so test dictionaries persist between runs while staying separate from the user's normal profile.
- Support a `-TestProfile` build mode whose EXE selects the same persistent isolated profile directly, with no launcher required.
- Test importing a synthetic Yomitan ZIP while a brief SQLite write lock is held.
- Provide reproducible build and PE dependency staging scripts.

No claim of byte-for-byte reproduction of the 19 October personal executable. Interactive media/Anki/audio testing remains separate from automated checks.
