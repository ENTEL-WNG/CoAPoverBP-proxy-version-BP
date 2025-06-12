## RUNNING THE PROJECT FOR THE FIRST TIME

```bash
make posix
sudo apt install python3-pip
sudo pip install ud3tn
sudo pip install aiocoap
sudo pip install aioconsole
```

## IF YOU ENCOUTER THE ERROR "make[1]: Nothing to be done for 'posix'.", RUN

```bash
make clean
make posix
```

## FOR EACH DEPLOYMENT, INSIDE THE UD3TN FOLDER RUN

```bash
build/posix/ud3tn --node-id dtn://a.dtn/ --aap-port 4242 --aap2-socket ud3tn-a.aap2.socket --cla "tcpclv3:*,4224"
```

```bash
build/posix/ud3tn --node-id dtn://b.dtn/ --aap-port 4243 --aap2-socket ud3tn-b.aap2.socket --cla "tcpclv3:*,4225"
```

```bash
build/posix/ud3tn --node-id dtn://c.dtn/ --aap-port 4244 --aap2-socket ud3tn-c.aap2.socket --cla "sqlite:ud3tn-c.sqlite;tcpclv3:*,4226" --external-dispatch
```

```bash
aap2-bdm-ud3tn-routing -vv --socket ud3tn-c.aap2.socket
```

```bash
build/posix/ud3tn --node-id dtn://d.dtn/ --aap-port 4245 --aap2-socket ud3tn-d.aap2.socket --cla "tcpclv3:*,4227"
```

```bash
aap2-config --socket ud3tn-a.aap2.socket --schedule 1 600 100000 dtn://c.dtn/ --reaches dtn://b.dtn/ --reaches dtn://d.dtn/  tcpclv3:localhost:4226
aap2-config --socket ud3tn-c.aap2.socket --schedule 30 600 100000 dtn://d.dtn/ --reaches dtn://b.dtn/ tcpclv3:localhost:4227 
aap2-config --socket ud3tn-d.aap2.socket --schedule 1 600 100000 dtn://c.dtn/ --reaches dtn://a.dtn/ tcpclv3:localhost:4226
aap2-config --socket ud3tn-d.aap2.socket --reaches dtn://b.dtn/rec --schedule 1 600 100000  dtn://b.dtn/ tcpclv3:localhost:4225
aap2-config --socket ud3tn-c.aap2.socket --reaches dtn://a.dtn/rec --schedule 1 600 100000 dtn://a.dtn/ tcpclv3:localhost:4224 
aap2-config --socket ud3tn-b.aap2.socket --schedule 1 600 100000 dtn://d.dtn/ --reaches dtn://c.dtn/ --reaches dtn://a.dtn/ tcpclv3:localhost:4227
```

```bash
python3 NodeAaap2.py
```

```bash
python3 NodeBaap2.py
```

```bash
python3 NodeDaap2.py
```

## TO INTERACT WITH THE BDM AND PERSISTENT STORAGE ON NODE C, CHECK

```bash
sqlite3 ud3tn-c.sqlite \
  "SELECT * FROM bundles;"

sqlite3 ud3tn-c.sqlite \
  "SELECT COUNT(*) FROM bundles;"

sqlite3 ud3tn-c.sqlite \
  "DELETE FROM bundles WHERE creation_timestamp = ;"

aap2-storage-agent --socket ud3tn-c.aap2.socket --storage-agent-eid "dtn://c.dtn/sqlite" push --dest-eid-glob "*"
```

## TOPOLOGY

The Topology can be sen in Topology.png

## AUTHORS

- Michael Karpov <michael.karpov@estudiantat.upc.edu> — Initial author and main developer
- Anna Calveras Supervisor

## FUNDING

This research was funded in part by the Spanish MCIU/AEI/10.13039/501100011033/ FEDER/UE through project PID2023-146378NB-I00, and by Secretaria d'Universitats i Recerca del departament d'Empresa i Coneixement de la Generalitat de Catalunya with the grant number 2021 SGR 00330

## LICENSE

This project incorporates code from several open source libraries and includes original code and modifications.

**This Project's Code and Modifications (including modifications to aiocoap):**

This code is licensed under the GNU Affero General Public License Version 3 (AGPLv3). See the `LICENSE` file in the root of this repository for the full text.

This project includes modified files from the aiocoap library(https://github.com/chrysn/aiocoap), originally developed by Christian Amsüss and contributors.

Modifications were made to support implement the main ideas of ietf draft "draft-gomez-core-coap-bp-03".

All changes are clearly marked in the source files with inline comments "# experimental for draft-gomez-core-coap-bp-03".

**aiocoap Library:**

This project includes code from the aiocoap library, which is licensed under the BSD 3-Clause License. The full text of this license can be found in the `LICENSE` folder in the aiocoap library.

**µD3TN Library:**

This project includes code from the µD3TN library, which is licensed under the AGPLv3 License. The full text of this license can be found in the `LICENSE` file in the ud3tn library.

## ACKNOWLEDGEMENTS

This work is partly based on the Bachelors Thesis of Max Lampurlanés Rosell. The work can be accessed under: https://upcommons.upc.edu/handle/2117/425606

The author gratefully acknowledges Anna Calveras and Carles Gómez for their invaluable feedback and guidance during this work.
