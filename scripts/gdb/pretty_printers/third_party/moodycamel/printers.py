import re
import warnings

import gdb

# https://github.com/PyCQA/pycodestyle/issues/728
warnings.filterwarnings('ignore', category=FutureWarning, message='Possible nested set at position')


def atomic_load(val: gdb.Value):
    return val.cast(val.type.template_argument(0))


class ConcurrentQueuePrinter:
    def __init__(self, val: gdb.Value):
        self.val = val
        self.T_type = val.type.template_argument(0)
        self.producer_list_tail = atomic_load(val['producerListTail'])
        self.catcher_name = f'moodycamel_extractor::AddressCatcher<{self.T_type.sizeof}ul, {self.T_type.alignof}ul>'
        self.catchers_queue = gdb.parse_and_eval(
            f'(moodycamel::ConcurrentQueue<{self.catcher_name}, moodycamel::ConcurrentQueueDefaultTraits>*)({val.address})'
        )

    def to_string(self):
        return f'moodycamel::ConcurrentQueue'

    def display_hint(self):
        return 'array'

    def children(self):
        index = 0
        producer = self.producer_list_tail
        seen: set[gdb.Value] = set()
        while producer != 0 and producer not in seen:
            seen.add(producer)
            for addr in self._process_producer(producer):
                yield (f'[{index}]', gdb.parse_and_eval(f'*({self.T_type.pointer()}){addr}'))
                index += 1
            producer = producer['next'].cast(gdb.lookup_type(f'{self.val.type}::ProducerBase').pointer())

    def _process_producer(self, producer: gdb.Value) -> list[str]:
        is_explicit = bool(producer['isExplicit'])
        head = int(atomic_load(producer['headIndex']))
        tail = int(atomic_load(producer['tailIndex']))
        if head == tail:
            return []
        gdb.execute(f'set {self.catcher_name}::destroys_until_throw = {tail - head - 1}')
        gdb.execute(f'call {self.catcher_name}::reset_addresses()')
        producer_name = 'ExplicitProducer' if is_explicit else 'ImplicitProducer'
        producer_fullname = f'{self.catchers_queue.type.target()}::{producer_name}'
        destructor_name = f'{producer_fullname}::~{producer_name}'
        destructor = gdb.lookup_global_symbol(destructor_name)
        assert destructor is not None, f'Destructor {destructor_name} not found'
        producer = gdb.parse_and_eval(f'({producer_fullname}*){producer}')
        gdb.execute(f'set $producer_vptr = (({producer.type}){producer})._vptr$ProducerBase')
        try:
            destructor.value()(producer)
        except Exception as e:
            if 'unhandled C++ exception' not in str(e):
                raise
        finally:
            gdb.execute(f'set (({producer.type}){producer})._vptr$ProducerBase = $producer_vptr', to_string=True)
        addresses_unparsed = str(gdb.parse_and_eval(f'{self.catcher_name}::addresses'))
        return re.findall(r'(?:\[[0-9]+\] = )?(0x[0-9a-f]+)', addresses_unparsed)


def register_printers():
    pp = gdb.printing.RegexpCollectionPrettyPrinter('moodycamel::ConcurrentQueue')
    pp.add_printer('ConcurrentQueue', '^moodycamel::ConcurrentQueue<.*>$', ConcurrentQueuePrinter)
    gdb.printing.register_pretty_printer(gdb.current_objfile(), pp)


register_printers()
