# Self-Playing Piano
## What is this Project?
Essentially, it's a piano that mimics the movement of a music piece being played on the piano. 
It doesn't actually make sounds just mimics the motion.

## Why did I make this?
I think this could be a really cool piece of "furniture" to put on your desk or your shelf. It's
a great way to learn about sliding gear assemblies, lead screw gantries, and the like.

## How does it work?
There are 22 keys (3 octaves of natural keys), all of them free-rotating on an M4 threaded rod. They
have part of a gear attached to them. There is a corresponding gear attached to a servo, which is mounted
on a lead screw gantry driven by a stepper motor. The servo is moved to the intended key and actuates the
gear, which in turn rotates the key.

### CAD Assembly
_minus the electronics_
<img width="1152" height="1028" alt="image" src="https://github.com/user-attachments/assets/99669ca8-0885-4a5e-955c-d6b0e6fa15ab" />

## How do you play a piece on it?
Piece Requirements:
- The piece needs to only be playing one note at a time
- Needs to be formatted in the correct JSON file format
- Needs to have a range <= 3 Octaves

Upload the json file to the website and then press play.
