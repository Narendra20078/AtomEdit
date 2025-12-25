# AtomEdit
SyncText is a lock-free, CRDT-based collaborative text editor that enables multiple users to edit a shared document concurrently. It uses POSIX shared memory, message queues, and timestamp-based conflict resolution to ensure eventual consistency without central locking, similar to real-time editors like Google Docs.
